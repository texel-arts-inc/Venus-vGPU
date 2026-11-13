#ifndef VIRTUALIZATION_VENUS_PM_CONTROLLER_H
#define VIRTUALIZATION_VENUS_PM_CONTROLLER_H

#include "darwin-power.h"
#include "venus-pm-host.h"

struct VenusPMController;

/* All operations run on the main loop. The guest transport queues whole frames. */
struct VenusPMControllerOps {
    bool (*guest_send)(void *opaque, const struct VenusPMFrame *frame);
    bool (*pause_and_gate)(void *opaque);
    bool (*restore_guest)(void *opaque, bool after_sleep);
    void (*ack)(void *opaque, enum DarwinPowerEvent event, uintptr_t token,
                bool allow);
    void (*arm_timeout)(void *opaque, unsigned delay_ms);
    void (*fault)(void *opaque, const char *reason);
};

struct VenusPMController *
venus_pm_controller_open(const char *path, VenusPMWatch *watch,
                         const struct VenusPMControllerOps *ops, void *opaque);
void venus_pm_controller_close(struct VenusPMController *controller);
void venus_pm_controller_power(struct VenusPMController *controller,
                               enum DarwinPowerEvent event, uintptr_t token);
void venus_pm_controller_guest(struct VenusPMController *controller,
                               const struct VenusPMFrame *frame);
void venus_pm_controller_guest_lost(struct VenusPMController *controller);
void venus_pm_controller_timeout(struct VenusPMController *controller);

#endif
