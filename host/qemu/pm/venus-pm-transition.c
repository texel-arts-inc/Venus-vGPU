#include "venus-pm-transition.h"

static bool matches(const struct VenusPMTransition *pm,
                    const struct VenusPMFrame *message, enum VenusPMOpcode opcode)
{
    return pm->cookie && pm->id && message->opcode == opcode &&
           message->cookie == pm->cookie && message->transition == pm->id &&
           message->status == 0;
}

static void frame(const struct VenusPMTransition *pm, enum VenusPMOpcode opcode,
                  struct VenusPMFrame *output)
{
    *output = (struct VenusPMFrame) {
        .opcode = opcode,
        .cookie = pm->cookie,
        .transition = pm->id,
    };
}

bool venus_pm_begin(struct VenusPMTransition *pm, size_t clients,
                    struct VenusPMFrame *prepare)
{
    if (pm->phase != VENUS_PM_RUNNING || !pm->cookie || pm->id == UINT64_MAX) {
        return false;
    }
    pm->id++;
    pm->pending_clients = clients;
    pm->host_access_verified = false;
    pm->phase = clients ? VENUS_PM_DRAINING : VENUS_PM_GUEST_SUSPENDING;
    frame(pm, VENUS_PM_PREPARE, prepare);
    return true;
}

bool venus_pm_client_drained(struct VenusPMTransition *pm,
                              struct VenusPMClient *client,
                              const struct VenusPMFrame *message)
{
    if (!matches(pm, message, VENUS_PM_DRAINED)) {
        return false;
    }
    if (client->drained_transition == pm->id) {
        return true;
    }
    if (pm->phase != VENUS_PM_DRAINING || !pm->pending_clients) {
        return false;
    }
    client->drained_transition = pm->id;
    if (--pm->pending_clients == 0) {
        pm->phase = VENUS_PM_GUEST_SUSPENDING;
    }
    return true;
}

bool venus_pm_guest_quiesced(struct VenusPMTransition *pm,
                              const struct VenusPMFrame *message)
{
    if (!matches(pm, message, VENUS_PM_QUIESCED)) {
        return false;
    }
    if (pm->phase == VENUS_PM_SUSPENDED) {
        return true;
    }
    if (pm->phase != VENUS_PM_GUEST_SUSPENDING) {
        return false;
    }
    pm->phase = VENUS_PM_SUSPENDED;
    return true;
}

bool venus_pm_host_restored(struct VenusPMTransition *pm,
                             struct VenusPMFrame *resume)
{
    if (pm->phase != VENUS_PM_SUSPENDED && pm->phase != VENUS_PM_RESUMING &&
        pm->phase != VENUS_PM_ABORTING) {
        return false;
    }
    pm->host_access_verified = true;
    if (pm->phase == VENUS_PM_ABORTING) {
        frame(pm, VENUS_PM_ABORT, resume);
    } else {
        pm->phase = VENUS_PM_RESUMING;
        frame(pm, VENUS_PM_RESUME, resume);
    }
    return true;
}

bool venus_pm_abort(struct VenusPMTransition *pm)
{
    if (pm->phase == VENUS_PM_RUNNING) {
        return false;
    }
    if (pm->phase != VENUS_PM_ABORTING) {
        pm->phase = VENUS_PM_ABORTING;
        pm->host_access_verified = false;
    }
    return true;
}

bool venus_pm_guest_ready(struct VenusPMTransition *pm,
                           const struct VenusPMFrame *message,
                           struct VenusPMFrame *ready)
{
    if (!pm->host_access_verified || !matches(pm, message, VENUS_PM_READY) ||
        (pm->phase != VENUS_PM_RESUMING && pm->phase != VENUS_PM_ABORTING)) {
        return false;
    }
    pm->phase = VENUS_PM_RUNNING;
    pm->pending_clients = 0;
    frame(pm, VENUS_PM_READY, ready);
    return true;
}
