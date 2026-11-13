#include "qemu/osdep.h"

#include "apple-vfio-pm-controller.h"
#include "venus-pm-controller.h"
#include "chardev/char-fe.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/runstate.h"

struct AppleVFIOPM {
    VFIOApplePCIDevice *dev;
    AppleVFIODMACheck *verify_dma;
    struct VenusPMController *controller;
    struct DarwinPower *power;
    QEMUBH *power_bh;
    GAsyncQueue *power_events;
    QEMUTimer *deadline;
    CharFrontend guest;
    guint guest_write_watch;
    uint8_t input[VENUS_PM_FRAME_SIZE];
    uint8_t output[2 * VENUS_PM_FRAME_SIZE];
    size_t input_size;
    size_t output_size;
};

struct PowerEvent {
    enum DarwinPowerEvent event;
    uintptr_t token;
};

static void discard_guest_output(struct AppleVFIOPM *pm)
{
    if (pm->guest_write_watch) {
        g_source_remove(pm->guest_write_watch);
        pm->guest_write_watch = 0;
    }
    pm->output_size = 0;
}

static gboolean write_guest(void *unused, GIOCondition condition, void *opaque)
{
    struct AppleVFIOPM *pm = opaque;
    int count;

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        pm->guest_write_watch = 0;
        pm->output_size = 0;
        venus_pm_controller_guest_lost(pm->controller);
        return G_SOURCE_REMOVE;
    }
    count = qemu_chr_fe_write(&pm->guest, pm->output, pm->output_size);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
        return G_SOURCE_CONTINUE;
    }
    if (count < 0) {
        pm->guest_write_watch = 0;
        pm->output_size = 0;
        venus_pm_controller_guest_lost(pm->controller);
        return G_SOURCE_REMOVE;
    }
    pm->output_size -= count;
    if (pm->output_size) {
        memmove(pm->output, pm->output + count, pm->output_size);
        return G_SOURCE_CONTINUE;
    }
    pm->guest_write_watch = 0;
    return G_SOURCE_REMOVE;
}

static bool send_guest(void *opaque, const struct VenusPMFrame *frame)
{
    struct AppleVFIOPM *pm = opaque;

    if (!qemu_chr_fe_backend_open(&pm->guest) ||
        pm->output_size > sizeof(pm->output) - VENUS_PM_FRAME_SIZE ||
        !venus_pm_encode(pm->output + pm->output_size, frame)) {
        return false;
    }
    pm->output_size += VENUS_PM_FRAME_SIZE;
    if (!pm->guest_write_watch) {
        pm->guest_write_watch = qemu_chr_fe_add_watch(
            &pm->guest, G_IO_OUT | G_IO_HUP | G_IO_ERR, write_guest, pm);
        if (!pm->guest_write_watch) {
            pm->output_size = 0;
            return false;
        }
    }
    return true;
}

static int guest_can_read(void *opaque)
{
    struct AppleVFIOPM *pm = opaque;

    return VENUS_PM_FRAME_SIZE - pm->input_size;
}

static void guest_read(void *opaque, const uint8_t *data, int count)
{
    struct AppleVFIOPM *pm = opaque;
    struct VenusPMFrame frame;

    assert(count >= 0 && count <= guest_can_read(pm));
    memcpy(pm->input + pm->input_size, data, count);
    pm->input_size += count;
    if (pm->input_size == VENUS_PM_FRAME_SIZE) {
        pm->input_size = 0;
        if (!venus_pm_decode(&frame, pm->input)) {
            venus_pm_controller_guest_lost(pm->controller);
            return;
        }
        venus_pm_controller_guest(pm->controller, &frame);
    }
}

static void guest_event(void *opaque, QEMUChrEvent event)
{
    struct AppleVFIOPM *pm = opaque;

    if (event == CHR_EVENT_CLOSED) {
        discard_guest_output(pm);
        pm->input_size = 0;
        venus_pm_controller_guest_lost(pm->controller);
    }
}

static bool pause_guest(void *opaque)
{
    struct AppleVFIOPM *pm = opaque;
    Error *err = NULL;

    assert(bql_locked());
    if (!runstate_is_running() || vm_stop(RUN_STATE_PAUSED) < 0) {
        return false;
    }
    if (!apple_vfio_pm_gate(pm->dev, &err)) {
        error_report_err(err);
        return false;
    }
    return true;
}

static bool restore_guest(void *opaque, bool after_sleep)
{
    struct AppleVFIOPM *pm = opaque;
    Error *err = NULL;

    assert(bql_locked());
    if (!apple_vfio_pm_validate_pci(pm->dev, &err) ||
        (after_sleep && !pm->verify_dma(pm->dev, &err))) {
        if (err) {
            error_report_err(err);
        }
        return false;
    }
    apple_vfio_pm_enable_after_dma_check(pm->dev);
    vm_start();
    return true;
}

static void power_ack(void *opaque, enum DarwinPowerEvent event,
                      uintptr_t token, bool allow)
{
    struct AppleVFIOPM *pm = opaque;
    int result = darwin_power_ack(pm->power, event, token, allow);

    if (result != 0) {
        warn_report("vfio-apple: power notification acknowledgment failed (%#x)",
                    result);
    }
}

static void arm_timeout(void *opaque, unsigned delay_ms)
{
    struct AppleVFIOPM *pm = opaque;

    if (delay_ms) {
        timer_mod(pm->deadline, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + delay_ms);
    } else {
        timer_del(pm->deadline);
    }
}

static void on_timeout(void *opaque)
{
    struct AppleVFIOPM *pm = opaque;

    venus_pm_controller_timeout(pm->controller);
}

static void power_fault(void *opaque, const char *reason)
{
    struct AppleVFIOPM *pm = opaque;

    /* A forced sleep can outlast the host's acknowledgment deadline. */
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
    apple_vfio_pm_emergency_gate(pm->dev);
    error_report("vfio-apple: %s", reason);
}

static void post_power_event(void *opaque, enum DarwinPowerEvent event,
                             uintptr_t token)
{
    struct AppleVFIOPM *pm = opaque;
    struct PowerEvent *message = g_new(struct PowerEvent, 1);

    *message = (struct PowerEvent){ .event = event, .token = token };
    g_async_queue_push(pm->power_events, message);
    qemu_bh_schedule(pm->power_bh);
}

static void power_events(void *opaque)
{
    struct AppleVFIOPM *pm = opaque;
    struct PowerEvent *message;

    while ((message = g_async_queue_try_pop(pm->power_events))) {
        venus_pm_controller_power(pm->controller, message->event, message->token);
        g_free(message);
    }
}

struct AppleVFIOPM *apple_vfio_pm_start(VFIOApplePCIDevice *dev,
                                      const char *icd_socket,
                                      const char *guest_chardev,
                                      AppleVFIODMACheck *verify_dma,
                                      Error **errp)
{
    static const struct VenusPMControllerOps ops = {
        .guest_send = send_guest,
        .pause_and_gate = pause_guest,
        .restore_guest = restore_guest,
        .ack = power_ack,
        .arm_timeout = arm_timeout,
        .fault = power_fault,
    };
    struct AppleVFIOPM *pm;
    Chardev *guest;

    if (!verify_dma) {
        error_setg(errp, "vfio-apple: a wake DMA validation backend is required");
        return NULL;
    }
    guest = qemu_chr_find(guest_chardev);
    if (!guest) {
        error_setg(errp, "vfio-apple: unknown guest power chardev '%s'", guest_chardev);
        return NULL;
    }
    pm = g_new0(struct AppleVFIOPM, 1);
    pm->dev = dev;
    pm->verify_dma = verify_dma;
    if (!qemu_chr_fe_init(&pm->guest, guest, errp)) {
        g_free(pm);
        return NULL;
    }
    pm->power_events = g_async_queue_new_full(g_free);
    pm->deadline = timer_new_ms(QEMU_CLOCK_REALTIME, on_timeout, pm);
    pm->power_bh = qemu_bh_new(power_events, pm);
    pm->controller = venus_pm_controller_open(icd_socket, qemu_set_fd_handler,
                                              &ops, pm);
    if (!pm->controller) {
        error_setg_errno(errp, errno, "vfio-apple: cannot open Venus power socket");
        apple_vfio_pm_stop(pm);
        return NULL;
    }
    pm->power = darwin_power_start(post_power_event, pm);
    if (!pm->power) {
        error_setg(errp, "vfio-apple: cannot register macOS power notifications");
        apple_vfio_pm_stop(pm);
        return NULL;
    }
    qemu_chr_fe_set_handlers(&pm->guest, guest_can_read, guest_read, guest_event,
                             NULL, pm, NULL, true);
    return pm;
}

void apple_vfio_pm_stop(struct AppleVFIOPM *pm)
{
    qemu_chr_fe_set_handlers(&pm->guest, NULL, NULL, NULL, NULL, NULL, NULL, false);
    discard_guest_output(pm);
    if (pm->controller) {
        venus_pm_controller_close(pm->controller);
    }
    darwin_power_stop(pm->power);
    qemu_bh_delete(pm->power_bh);
    g_async_queue_unref(pm->power_events);
    timer_free(pm->deadline);
    qemu_chr_fe_deinit(&pm->guest, false);
    g_free(pm);
}
