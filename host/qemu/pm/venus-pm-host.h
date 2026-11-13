#ifndef VIRTUALIZATION_VENUS_PM_HOST_H
#define VIRTUALIZATION_VENUS_PM_HOST_H

#include "venus-pm-transition.h"

struct VenusPMHost;

/* All calls and fd callbacks run on QEMU's main loop. A NULL pair unwatches. */
typedef void VenusPMWatch(int fd, void (*read)(void *), void (*write)(void *),
                          void *opaque);

enum VenusPMHostEvent {
    VENUS_PM_HOST_CLIENTS_DRAINED,
    VENUS_PM_HOST_CLIENT_FAILED,
};

/* The callback may start a guest operation. It must not destroy the host. */
typedef void VenusPMHostNotify(void *opaque, enum VenusPMHostEvent event);

struct VenusPMHost *venus_pm_host_open(const char *path, VenusPMWatch *watch,
                                      VenusPMHostNotify *notify, void *opaque);
void venus_pm_host_close(struct VenusPMHost *host);
/* A zero-client transition reports CLIENTS_DRAINED before prepare returns. */
bool venus_pm_host_prepare(struct VenusPMHost *host, struct VenusPMFrame *guest);
bool venus_pm_host_quiesced(struct VenusPMHost *host,
                            const struct VenusPMFrame *guest);
bool venus_pm_host_abort(struct VenusPMHost *host);
bool venus_pm_host_restored_access(struct VenusPMHost *host,
                                   struct VenusPMFrame *guest);
bool venus_pm_host_ready(struct VenusPMHost *host,
                         const struct VenusPMFrame *guest);

#endif
