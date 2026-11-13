#include "venus-pm-controller.h"

#include <stdlib.h>

#define PREPARE_TIMEOUT_MS 20000
#define RECOVERY_TIMEOUT_MS 75000

enum Stage {
    RUNNING,
    DRAINING,
    SUSPENDING,
    PARKED,
    RECOVERING,
    FAULTED,
};

struct VenusPMController {
    struct VenusPMHost *host;
    struct VenusPMControllerOps ops;
    void *opaque;
    enum Stage stage;
    struct VenusPMFrame transition;
    bool idle_pending;
    uintptr_t idle_token;
    bool sleep_pending;
    uintptr_t sleep_token;
};

static void acknowledge(struct VenusPMController *c, bool allow_idle,
                        bool allow_sleep)
{
    if (c->idle_pending) {
        c->idle_pending = false;
        c->ops.ack(c->opaque, DARWIN_POWER_IDLE_REQUEST, c->idle_token,
                   allow_idle);
    }
    if (allow_sleep && c->sleep_pending) {
        c->sleep_pending = false;
        c->ops.ack(c->opaque, DARWIN_POWER_WILL_SLEEP, c->sleep_token, true);
    }
}

static void fault(struct VenusPMController *c, const char *reason)
{
    c->stage = FAULTED;
    c->ops.arm_timeout(c->opaque, 0);
    acknowledge(c, false, false);
    c->ops.fault(c->opaque, reason);
}

static void recover(struct VenusPMController *c, bool aborted, bool after_sleep)
{
    bool guest_prepared = c->stage != DRAINING;
    struct VenusPMFrame frame;

    if (c->stage == PARKED && !c->ops.restore_guest(c->opaque, after_sleep)) {
        fault(c, "GPU restoration failed. VM remains paused.");
        return;
    }
    c->stage = RECOVERING;
    if ((aborted && !venus_pm_host_abort(c->host)) ||
        !venus_pm_host_restored_access(c->host, &frame)) {
        fault(c, "Invalid Venus recovery transition.");
        return;
    }
    if (!guest_prepared) {
        /* PREPARE never left QEMU. Only the host ICDs must reopen. */
        frame.opcode = VENUS_PM_READY;
        venus_pm_controller_guest(c, &frame);
        return;
    }
    c->ops.arm_timeout(c->opaque, RECOVERY_TIMEOUT_MS);
    if (!c->ops.guest_send(c->opaque, &frame)) {
        fault(c, "Unable to send Venus recovery to the guest.");
    }
}

static void host_event(void *opaque, enum VenusPMHostEvent event)
{
    struct VenusPMController *c = opaque;

    if (event == VENUS_PM_HOST_CLIENTS_DRAINED && c->stage == DRAINING) {
        c->stage = SUSPENDING;
        if (!c->ops.guest_send(c->opaque, &c->transition)) {
            fault(c, "Unable to send Venus PREPARE to the guest.");
        }
    } else if (event == VENUS_PM_HOST_CLIENT_FAILED &&
               (c->stage == DRAINING || c->stage == SUSPENDING)) {
        acknowledge(c, false, false);
        if (!c->sleep_pending) {
            recover(c, true, false);
        }
    }
}

struct VenusPMController *
venus_pm_controller_open(const char *path, VenusPMWatch *watch,
                         const struct VenusPMControllerOps *ops, void *opaque)
{
    struct VenusPMController *c = calloc(1, sizeof(*c));

    if (!c) {
        return NULL;
    }
    c->ops = *ops;
    c->opaque = opaque;
    c->host = venus_pm_host_open(path, watch, host_event, c);
    if (!c->host) {
        free(c);
        return NULL;
    }
    return c;
}

void venus_pm_controller_close(struct VenusPMController *c)
{
    c->ops.arm_timeout(c->opaque, 0);
    acknowledge(c, false, false);
    venus_pm_host_close(c->host);
    free(c);
}

void venus_pm_controller_power(struct VenusPMController *c,
                               enum DarwinPowerEvent event, uintptr_t token)
{
    switch (event) {
    case DARWIN_POWER_IDLE_REQUEST:
        if (c->stage == PARKED) {
            c->ops.ack(c->opaque, event, token, true);
            return;
        }
        if (c->stage != RUNNING) {
            c->ops.ack(c->opaque, event, token, false);
            return;
        }
        c->idle_pending = true;
        c->idle_token = token;
        break;
    case DARWIN_POWER_WILL_SLEEP:
        c->sleep_pending = true;
        c->sleep_token = token;
        if (c->stage == PARKED) {
            acknowledge(c, true, true);
        }
        if (c->stage != RUNNING) {
            return;
        }
        break;
    case DARWIN_POWER_SLEEP_CANCELED:
        c->idle_pending = false;
        if (c->sleep_pending) {
            return;
        }
        if (c->stage == DRAINING || c->stage == SUSPENDING || c->stage == PARKED) {
            recover(c, true, false);
        }
        return;
    case DARWIN_POWER_HAS_WOKEN:
        if (c->stage == PARKED) {
            c->sleep_pending = false;
            recover(c, false, true);
        } else if (c->stage != RUNNING && c->stage != FAULTED) {
            fault(c, "Mac slept before Venus reached its safe checkpoint.");
        }
        return;
    }
    c->stage = DRAINING;
    c->ops.arm_timeout(c->opaque, PREPARE_TIMEOUT_MS);
    if (!venus_pm_host_prepare(c->host, &c->transition)) {
        fault(c, "Unable to begin Venus power transition.");
    }
}

void venus_pm_controller_guest(struct VenusPMController *c,
                               const struct VenusPMFrame *frame)
{
    if (frame->cookie != c->transition.cookie ||
        frame->transition != c->transition.transition) {
        return;
    }
    if (c->stage == SUSPENDING && frame->opcode == VENUS_PM_QUIESCED &&
        venus_pm_host_quiesced(c->host, frame)) {
        if (!c->ops.pause_and_gate(c->opaque)) {
            fault(c, "Unable to pause the VM and gate GPU access.");
            return;
        }
        c->stage = PARKED;
        c->ops.arm_timeout(c->opaque, 0);
        acknowledge(c, true, true);
    } else if (c->stage == RECOVERING && venus_pm_host_ready(c->host, frame)) {
        c->stage = RUNNING;
        c->ops.arm_timeout(c->opaque, 0);
    } else if (frame->opcode == VENUS_PM_FAILED) {
        if (c->stage == SUSPENDING && !c->sleep_pending) {
            acknowledge(c, false, false);
            recover(c, true, false);
        } else {
            fault(c, "Guest reported a Venus power transition failure.");
        }
    }
}

void venus_pm_controller_guest_lost(struct VenusPMController *c)
{
    if (c->stage != RUNNING) {
        fault(c, "Venus guest power channel closed during a transition.");
    }
}

void venus_pm_controller_timeout(struct VenusPMController *c)
{
    if ((c->stage == DRAINING || c->stage == SUSPENDING) && !c->sleep_pending) {
        acknowledge(c, false, false);
        recover(c, true, false);
    } else if (c->stage != RUNNING && c->stage != PARKED) {
        fault(c, "Venus power transition timed out.");
    }
}
