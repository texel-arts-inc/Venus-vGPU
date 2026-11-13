#include "darwin-power.h"

#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>

struct DarwinPower {
    io_connect_t connection;
    io_object_t notifier;
    IONotificationPortRef port;
    dispatch_queue_t queue;
    DarwinPowerNotify notify;
    void *opaque;
};

static void handle_power_event(void *opaque, io_service_t service,
                               natural_t message, void *argument)
{
    struct DarwinPower *power = opaque;
    enum DarwinPowerEvent event;
    (void)service;

    switch (message) {
    case kIOMessageCanSystemSleep:
        event = DARWIN_POWER_IDLE_REQUEST;
        break;
    case kIOMessageSystemWillSleep:
        event = DARWIN_POWER_WILL_SLEEP;
        break;
    case kIOMessageSystemWillNotSleep:
        event = DARWIN_POWER_SLEEP_CANCELED;
        break;
    case kIOMessageSystemHasPoweredOn:
        event = DARWIN_POWER_HAS_WOKEN;
        break;
    default:
        return;
    }
    power->notify(power->opaque, event, (uintptr_t)argument);
}

struct DarwinPower *darwin_power_start(DarwinPowerNotify notify, void *opaque)
{
    struct DarwinPower *power;

    if (!notify) {
        return NULL;
    }
    power = calloc(1, sizeof(*power));
    if (!power) {
        return NULL;
    }
    power->notify = notify;
    power->opaque = opaque;
    power->connection = IORegisterForSystemPower(power, &power->port,
                                                  handle_power_event,
                                                  &power->notifier);
    if (!power->connection) {
        free(power);
        return NULL;
    }
    power->queue = dispatch_queue_create("com.texel-arts.venus-vgpu.power", DISPATCH_QUEUE_SERIAL);
    if (!power->queue) {
        IODeregisterForSystemPower(&power->notifier);
        IONotificationPortDestroy(power->port);
        IOServiceClose(power->connection);
        free(power);
        return NULL;
    }
    IONotificationPortSetDispatchQueue(power->port, power->queue);
    return power;
}

int darwin_power_ack(struct DarwinPower *power, enum DarwinPowerEvent event,
                     uintptr_t token, bool allow)
{
    if (event == DARWIN_POWER_IDLE_REQUEST && !allow) {
        return IOCancelPowerChange(power->connection, (intptr_t)token);
    }
    if (allow && (event == DARWIN_POWER_IDLE_REQUEST || event == DARWIN_POWER_WILL_SLEEP)) {
        return IOAllowPowerChange(power->connection, (intptr_t)token);
    }
    return kIOReturnBadArgument;
}

static void drain_queue(void *opaque)
{
    (void)opaque;
}

void darwin_power_stop(struct DarwinPower *power)
{
    if (power) {
        IONotificationPortSetDispatchQueue(power->port, NULL);
        IODeregisterForSystemPower(&power->notifier);
        dispatch_sync_f(power->queue, NULL, drain_queue);
        IONotificationPortDestroy(power->port);
        IOServiceClose(power->connection);
        dispatch_release(power->queue);
        free(power);
    }
}
