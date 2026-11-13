#ifndef RV_RENDERER_PM_H
#define RV_RENDERER_PM_H

#include "venus-pm-wire.h"

#include <poll.h>
#include <stddef.h>

enum rv_renderer_pm_phase {
    RV_RENDERER_RUNNING,
    RV_RENDERER_DRAINING,
    RV_RENDERER_DRAINED,
    RV_RENDERER_VERIFYING,
    RV_RENDERER_ABORTING,
    RV_RENDERER_FAILED,
};

struct rv_renderer_pm_ops {
    int (*drain)(void *renderer, uint64_t transition);
    int (*verify)(void *renderer, uint64_t transition);
    int (*abort)(void *renderer, uint64_t transition);
};

struct rv_renderer_pm {
    uint64_t cookie;
    uint64_t transition;
    enum rv_renderer_pm_phase phase;
    enum VenusPMOpcode failure;
    enum VenusPMOpcode released_by;
    bool enrolled;
    void *renderer;
    const struct rv_renderer_pm_ops *ops;
};

#define RV_RENDERER_PM_PATH "/run/venus/renderer-pm.sock"

void rv_renderer_pm_peer_closed(struct rv_renderer_pm *pm);
int rv_renderer_pm_request(struct rv_renderer_pm *pm, const struct VenusPMFrame *request,
                           struct VenusPMFrame *reply);
int rv_renderer_pm_progress(struct rv_renderer_pm *pm, struct VenusPMFrame *reply);

struct rv_renderer_pm_endpoint {
    struct rv_renderer_pm pm;
    int listener;
    int peer;
    const char *path;
    uint64_t deadline_ms;
    size_t received;
    size_t sent;
    bool reply_pending;
    uint8_t input[VENUS_PM_FRAME_SIZE];
    uint8_t output[VENUS_PM_FRAME_SIZE];
};

int rv_renderer_pm_open(struct rv_renderer_pm_endpoint *ep, const char *path);
void rv_renderer_pm_close(struct rv_renderer_pm_endpoint *ep);
void rv_renderer_pm_poll(const struct rv_renderer_pm_endpoint *ep, struct pollfd fds[2]);
void rv_renderer_pm_service(struct rv_renderer_pm_endpoint *ep, const struct pollfd fds[2],
                            uint64_t now_ms);

#endif
