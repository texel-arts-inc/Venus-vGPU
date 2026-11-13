#ifndef VIRTUALIZATION_VENUS_PM_TRANSITION_H
#define VIRTUALIZATION_VENUS_PM_TRANSITION_H

#include "venus-pm-wire.h"

#include <stddef.h>

/* The owner serializes calls on QEMU's main loop. One entry per enrolled ICD. */
struct VenusPMClient {
    uint64_t drained_transition;
};

enum VenusPMPhase {
    VENUS_PM_RUNNING,
    VENUS_PM_DRAINING,
    VENUS_PM_GUEST_SUSPENDING,
    VENUS_PM_SUSPENDED,
    VENUS_PM_RESUMING,
    VENUS_PM_ABORTING,
};

struct VenusPMTransition {
    uint64_t cookie;
    uint64_t id;
    enum VenusPMPhase phase;
    size_t pending_clients;
    bool host_access_verified;
};

/* Freeze enrollment while the phase differs from VENUS_PM_RUNNING. */
bool venus_pm_begin(struct VenusPMTransition *pm, size_t clients,
                    struct VenusPMFrame *prepare);
bool venus_pm_client_drained(struct VenusPMTransition *pm,
                              struct VenusPMClient *client,
                              const struct VenusPMFrame *message);
bool venus_pm_guest_quiesced(struct VenusPMTransition *pm,
                              const struct VenusPMFrame *message);
bool venus_pm_host_restored(struct VenusPMTransition *pm,
                             struct VenusPMFrame *resume);
bool venus_pm_abort(struct VenusPMTransition *pm);
bool venus_pm_guest_ready(struct VenusPMTransition *pm,
                           const struct VenusPMFrame *message,
                           struct VenusPMFrame *ready);

#endif
