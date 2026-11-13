#ifndef VIRTUALIZATION_DARWIN_POWER_H
#define VIRTUALIZATION_DARWIN_POWER_H

#include <stdbool.h>
#include <stdint.h>

struct DarwinPower;

enum DarwinPowerEvent {
    DARWIN_POWER_IDLE_REQUEST,
    DARWIN_POWER_WILL_SLEEP,
    DARWIN_POWER_SLEEP_CANCELED,
    DARWIN_POWER_HAS_WOKEN,
};

/* Invoked on a dedicated queue. Post work to QEMU's main loop and return. */
typedef void (*DarwinPowerNotify)(void *opaque, enum DarwinPowerEvent event,
                                  uintptr_t token);

struct DarwinPower *darwin_power_start(DarwinPowerNotify notify, void *opaque);

/* Only the idle-request event can be denied. Other events are notifications. */
int darwin_power_ack(struct DarwinPower *power, enum DarwinPowerEvent event,
                     uintptr_t token, bool allow);

/* Stop on the owner's thread. Discard posted callbacks before freeing opaque. */
void darwin_power_stop(struct DarwinPower *power);

#endif
