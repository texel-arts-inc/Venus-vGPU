#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "renderer-pm.h"

#include <errno.h>
#include <string.h>
#include <sys/random.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void
respond(const struct rv_renderer_pm *pm, struct VenusPMFrame *reply,
        enum VenusPMOpcode opcode, uint32_t status)
{
    *reply = (struct VenusPMFrame) {
        .opcode = opcode,
        .cookie = pm->cookie,
        .transition = pm->transition,
        .status = status,
    };
}

static int
listen_socket(const char *path)
{
#if defined(__linux__)
    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };
    const size_t length = strlen(path);
    const char *slash = strrchr(path, '/');
    if (length >= sizeof(address.sun_path) || path[0] != '/' || !slash ||
        slash == path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    char parent[sizeof(address.sun_path)];
    size_t parent_length = slash - path;
    memcpy(parent, path, parent_length);
    parent[parent_length] = 0;
    memcpy(address.sun_path, path, length + 1);
    struct stat directory;
    if (lstat(parent, &directory) < 0)
        return -1;
    if (!S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
        (directory.st_mode & 0777) != 0700) {
        errno = EACCES;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address))) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    if (chmod(path, 0600) || listen(fd, 1)) {
        int error = errno;
        unlink(path);
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
#else
    (void)path;
    errno = ENOSYS;
    return -1;
#endif
}

static int
accept_root(int listener)
{
#if defined(__linux__)
    int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return -1;
    struct ucred credential;
    socklen_t length = sizeof(credential);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) ||
        length != sizeof(credential) || credential.uid != 0) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
#else
    (void)listener;
    errno = ENOSYS;
    return -1;
#endif
}

void
rv_renderer_pm_peer_closed(struct rv_renderer_pm *pm)
{
    pm->enrolled = false;
    if (pm->phase != RV_RENDERER_RUNNING)
        pm->phase = RV_RENDERER_FAILED;
}

int
rv_renderer_pm_request(struct rv_renderer_pm *pm, const struct VenusPMFrame *request,
                       struct VenusPMFrame *reply)
{
    if (!pm->cookie || !pm->ops || !pm->ops->drain || !pm->ops->verify ||
        !pm->ops->abort || request->status)
        return -EINVAL;
    if (request->opcode == VENUS_PM_HELLO) {
        if (request->cookie || request->transition || pm->enrolled ||
            pm->phase != RV_RENDERER_RUNNING)
            return -EPROTO;
        pm->enrolled = true;
        respond(pm, reply, VENUS_PM_HELLO_ACK, 0);
        reply->transition = 0;
        return 1;
    }
    if (!pm->enrolled || request->cookie != pm->cookie || !request->transition)
        return -EPROTO;

    if (request->opcode == VENUS_PM_PREPARE && request->transition > pm->transition &&
        pm->phase == RV_RENDERER_RUNNING) {
        pm->transition = request->transition;
        pm->released_by = 0;
        pm->phase = RV_RENDERER_DRAINING;
        return 0;
    }
    if (request->transition != pm->transition)
        return 0;
    switch (request->opcode) {
    case VENUS_PM_PREPARE:
        if (pm->phase == RV_RENDERER_DRAINED) {
            respond(pm, reply, VENUS_PM_DRAINED, 0);
            return 1;
        }
        break;
    case VENUS_PM_RESUME:
        if (pm->phase == RV_RENDERER_DRAINED)
            pm->phase = RV_RENDERER_VERIFYING;
        else if (pm->phase == RV_RENDERER_RUNNING &&
                 pm->released_by == VENUS_PM_RESUME) {
            respond(pm, reply, VENUS_PM_READY, 0);
            return 1;
        }
        break;
    case VENUS_PM_ABORT:
        if (pm->phase == RV_RENDERER_RUNNING && pm->released_by == VENUS_PM_ABORT) {
            respond(pm, reply, VENUS_PM_READY, 0);
            return 1;
        }
        if (pm->phase == RV_RENDERER_DRAINING || pm->phase == RV_RENDERER_DRAINED ||
            (pm->phase == RV_RENDERER_FAILED && pm->failure == VENUS_PM_PREPARE))
            pm->phase = RV_RENDERER_ABORTING;
        break;
    default:
        return -EPROTO;
    }
    return 0;
}

int
rv_renderer_pm_progress(struct rv_renderer_pm *pm, struct VenusPMFrame *reply)
{
    int error;
    switch (pm->phase) {
    case RV_RENDERER_DRAINING:
        error = pm->ops->drain(pm->renderer, pm->transition);
        if (error == EAGAIN)
            return 0;
        if (!error) {
            pm->phase = RV_RENDERER_DRAINED;
            respond(pm, reply, VENUS_PM_DRAINED, 0);
            return 1;
        }
        pm->failure = VENUS_PM_PREPARE;
        break;
    case RV_RENDERER_VERIFYING:
        error = pm->ops->verify(pm->renderer, pm->transition);
        if (error == EAGAIN)
            return 0;
        pm->failure = VENUS_PM_RESUME;
        break;
    case RV_RENDERER_ABORTING:
        error = pm->ops->abort(pm->renderer, pm->transition);
        if (error == EAGAIN)
            return 0;
        pm->failure = VENUS_PM_ABORT;
        break;
    default:
        return 0;
    }

    if (error) {
        pm->phase = RV_RENDERER_FAILED;
        respond(pm, reply, VENUS_PM_FAILED, (uint32_t)error);
    } else {
        pm->phase = RV_RENDERER_RUNNING;
        pm->released_by = pm->failure;
        pm->failure = 0;
        respond(pm, reply, VENUS_PM_READY, 0);
    }
    return 1;
}

static bool
working(enum rv_renderer_pm_phase phase)
{
    return phase == RV_RENDERER_DRAINING || phase == RV_RENDERER_VERIFYING ||
           phase == RV_RENDERER_ABORTING;
}

static void
close_peer(struct rv_renderer_pm_endpoint *ep)
{
    if (ep->peer < 0)
        return;
    close(ep->peer);
    ep->peer = -1;
    ep->received = 0;
    ep->sent = 0;
    ep->reply_pending = false;
    rv_renderer_pm_peer_closed(&ep->pm);
}

int
rv_renderer_pm_open(struct rv_renderer_pm_endpoint *ep, const char *path)
{
    do {
        if (getentropy(&ep->pm.cookie, sizeof(ep->pm.cookie)))
            return -1;
    } while (!ep->pm.cookie);
    ep->listener = listen_socket(path);
    if (ep->listener < 0)
        return -1;
    ep->path = path;
    return 0;
}

void
rv_renderer_pm_close(struct rv_renderer_pm_endpoint *ep)
{
    close_peer(ep);
    if (ep->listener >= 0) {
        close(ep->listener);
        unlink(ep->path);
        ep->listener = -1;
    }
}

void
rv_renderer_pm_poll(const struct rv_renderer_pm_endpoint *ep, struct pollfd fds[2])
{
    fds[0] = (struct pollfd) {
        .fd = ep->peer < 0 ? ep->listener : -1,
        .events = POLLIN,
    };
    fds[1] = (struct pollfd) {
        .fd = ep->peer,
        .events = ep->reply_pending ? POLLOUT : POLLIN,
    };
}

static int
read_request(struct rv_renderer_pm_endpoint *ep, struct VenusPMFrame *reply)
{
    ssize_t count = recv(ep->peer, ep->input + ep->received,
                         sizeof(ep->input) - ep->received, 0);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    if (count <= 0)
        return -EPIPE;
    ep->received += (size_t)count;
    if (ep->received < sizeof(ep->input))
        return 0;
    ep->received = 0;
    struct VenusPMFrame request;
    if (!venus_pm_decode(&request, ep->input))
        return -EPROTO;
    return rv_renderer_pm_request(&ep->pm, &request, reply);
}

static bool
write_reply(struct rv_renderer_pm_endpoint *ep)
{
    ssize_t count = send(ep->peer, ep->output + ep->sent, sizeof(ep->output) - ep->sent,
                         MSG_NOSIGNAL);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return true;
    if (count <= 0)
        return false;
    ep->sent += (size_t)count;
    if (ep->sent == sizeof(ep->output)) {
        ep->sent = 0;
        ep->reply_pending = false;
    }
    return true;
}

void
rv_renderer_pm_service(struct rv_renderer_pm_endpoint *ep, const struct pollfd fds[2],
                       uint64_t now_ms)
{
    if (ep->peer < 0) {
        if (fds[0].revents & POLLIN)
            ep->peer = accept_root(ep->listener);
        return;
    }
    if (fds[1].revents & (POLLERR | POLLNVAL)) {
        close_peer(ep);
        return;
    }

    if (ep->reply_pending) {
        if ((fds[1].revents & (POLLOUT | POLLHUP)) && !write_reply(ep))
            close_peer(ep);
        return;
    }

    struct VenusPMFrame reply;
    const enum rv_renderer_pm_phase before = ep->pm.phase;
    int result = 0;
    if (fds[1].revents & (POLLIN | POLLHUP))
        result = read_request(ep, &reply);
    if (result < 0) {
        close_peer(ep);
        return;
    }
    if (working(ep->pm.phase) && !result) {
        if (ep->pm.phase != before)
            ep->deadline_ms = now_ms + 6500;
        if (now_ms >= ep->deadline_ms) {
            ep->pm.failure = ep->pm.phase == RV_RENDERER_DRAINING ? VENUS_PM_PREPARE :
                             ep->pm.phase == RV_RENDERER_VERIFYING ? VENUS_PM_RESUME :
                             VENUS_PM_ABORT;
            ep->pm.phase = RV_RENDERER_FAILED;
            respond(&ep->pm, &reply, VENUS_PM_FAILED, ETIMEDOUT);
            result = 1;
        } else {
            result = rv_renderer_pm_progress(&ep->pm, &reply);
        }
    }
    if (result == 1) {
        ep->reply_pending = venus_pm_encode(ep->output, &reply);
        if (!ep->reply_pending || !write_reply(ep))
            close_peer(ep);
    }
}
