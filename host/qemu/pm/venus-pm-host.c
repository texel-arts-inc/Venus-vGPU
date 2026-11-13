#include "venus-pm-host.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PM_CLIENTS 64
#define OUTPUT_CAPACITY (4 * VENUS_PM_FRAME_SIZE)

struct VenusPMPeer {
    struct VenusPMHost *host;
    struct VenusPMClient drain;
    int fd;
    bool enrolled;
    uint8_t input[VENUS_PM_FRAME_SIZE];
    size_t input_size;
    uint8_t output[OUTPUT_CAPACITY];
    size_t output_size;
};

struct VenusPMHost {
    struct VenusPMTransition transition;
    VenusPMWatch *watch;
    VenusPMHostNotify *notify;
    void *opaque;
    int listener;
    struct sockaddr_un address;
    struct VenusPMPeer peers[MAX_PM_CLIENTS];
};

static void read_peer(void *opaque);
static void write_peer(void *opaque);

static void watch_peer(struct VenusPMPeer *peer)
{
    peer->host->watch(peer->fd, read_peer,
                      peer->output_size ? write_peer : NULL, peer);
}

static void close_peer(struct VenusPMPeer *peer, bool report)
{
    bool active = peer->enrolled &&
                  peer->host->transition.phase != VENUS_PM_RUNNING;

    peer->host->watch(peer->fd, NULL, NULL, NULL);
    close(peer->fd);
    peer->fd = -1;
    peer->enrolled = false;
    if (report && active) {
        peer->host->notify(peer->host->opaque, VENUS_PM_HOST_CLIENT_FAILED);
    }
}

static bool queue_frame(struct VenusPMPeer *peer,
                        const struct VenusPMFrame *frame)
{
    if (peer->output_size > sizeof(peer->output) - VENUS_PM_FRAME_SIZE ||
        !venus_pm_encode(peer->output + peer->output_size, frame)) {
        return false;
    }
    peer->output_size += VENUS_PM_FRAME_SIZE;
    watch_peer(peer);
    return true;
}

static void write_peer(void *opaque)
{
    struct VenusPMPeer *peer = opaque;
    ssize_t size;

    if (peer->fd < 0 || !peer->output_size) {
        return;
    }
    size = send(peer->fd, peer->output, peer->output_size, 0);
    if (size < 0 && (errno == EAGAIN || errno == EINTR)) {
        return;
    }
    if (size <= 0) {
        close_peer(peer, true);
        return;
    }
    peer->output_size -= (size_t)size;
    memmove(peer->output, peer->output + size, peer->output_size);
    watch_peer(peer);
}

static bool handle_frame(struct VenusPMPeer *peer,
                         const struct VenusPMFrame *message)
{
    struct VenusPMHost *host = peer->host;
    struct VenusPMTransition *pm = &host->transition;

    if (!peer->enrolled) {
        struct VenusPMFrame ack = {
            .opcode = VENUS_PM_HELLO_ACK,
            .cookie = pm->cookie,
        };

        if (message->opcode != VENUS_PM_HELLO ||
            pm->phase != VENUS_PM_RUNNING || !queue_frame(peer, &ack)) {
            return false;
        }
        peer->enrolled = true;
        return true;
    }

    if (message->cookie != pm->cookie) {
        return false;
    }
    if (message->transition != pm->id) {
        return message->transition < pm->id;
    }
    if (message->opcode == VENUS_PM_FAILED && pm->phase != VENUS_PM_RUNNING) {
        host->notify(host->opaque, VENUS_PM_HOST_CLIENT_FAILED);
        return true;
    }
    if (message->opcode == VENUS_PM_DRAINED) {
        enum VenusPMPhase previous = pm->phase;

        if (venus_pm_client_drained(pm, &peer->drain, message) &&
            previous == VENUS_PM_DRAINING &&
            pm->phase == VENUS_PM_GUEST_SUSPENDING) {
            host->notify(host->opaque, VENUS_PM_HOST_CLIENTS_DRAINED);
        }
        return true;
    }
    return false;
}

static void read_peer(void *opaque)
{
    struct VenusPMPeer *peer = opaque;
    struct VenusPMFrame frame;
    ssize_t size;

    if (peer->fd < 0) {
        return;
    }
    size = recv(peer->fd, peer->input + peer->input_size,
                VENUS_PM_FRAME_SIZE - peer->input_size, 0);
    if (size < 0 && (errno == EAGAIN || errno == EINTR)) {
        return;
    }
    if (size <= 0) {
        close_peer(peer, true);
        return;
    }
    peer->input_size += (size_t)size;
    if (peer->input_size == VENUS_PM_FRAME_SIZE) {
        peer->input_size = 0;
        if (!venus_pm_decode(&frame, peer->input) || !handle_frame(peer, &frame)) {
            close_peer(peer, true);
        }
    }
}

static bool configure_socket(int fd)
{
    int flag = 1;

    return fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag)) == 0;
}

static void accept_peer(void *opaque)
{
    struct VenusPMHost *host = opaque;
    int fd = accept(host->listener, NULL, NULL);
    uid_t uid;
    gid_t gid;

    if (fd < 0) {
        return;
    }
    if (host->transition.phase == VENUS_PM_RUNNING && configure_socket(fd) &&
        getpeereid(fd, &uid, &gid) == 0 && uid == geteuid()) {
        for (size_t i = 0; i < MAX_PM_CLIENTS; i++) {
            if (host->peers[i].fd < 0) {
                host->peers[i] = (struct VenusPMPeer){ .host = host, .fd = fd };
                watch_peer(&host->peers[i]);
                return;
            }
        }
    }
    close(fd);
}

static bool private_parent(const char *path)
{
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char *separator;
    struct stat info;

    if (path[0] != '/' || strlen(path) >= sizeof(parent)) {
        return false;
    }
    strcpy(parent, path);
    separator = strrchr(parent, '/');
    if (separator == parent || !separator[1]) {
        return false;
    }
    *separator = '\0';
    return lstat(parent, &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == geteuid() && (info.st_mode & 0777) == 0700;
}

struct VenusPMHost *venus_pm_host_open(const char *path, VenusPMWatch *watch,
                                      VenusPMHostNotify *notify, void *opaque)
{
    struct VenusPMHost *host;
    int fd;
    int error;

    if (!path || !watch || !notify || !private_parent(path)) {
        errno = EINVAL;
        return NULL;
    }
    host = calloc(1, sizeof(*host));
    if (!host) {
        return NULL;
    }
    host->address.sun_family = AF_UNIX;
    strcpy(host->address.sun_path, path);
    host->address.sun_len = SUN_LEN(&host->address);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        free(host);
        return NULL;
    }
    if (!configure_socket(fd) ||
        bind(fd, (const struct sockaddr *)&host->address, host->address.sun_len) < 0) {
        error = errno;
        close(fd);
        free(host);
        errno = error;
        return NULL;
    }
    if (chmod(path, 0600) < 0 || listen(fd, MAX_PM_CLIENTS) < 0) {
        error = errno;
        unlink(path);
        close(fd);
        free(host);
        errno = error;
        return NULL;
    }
    host->listener = fd;
    host->watch = watch;
    host->notify = notify;
    host->opaque = opaque;
    do {
        arc4random_buf(&host->transition.cookie, sizeof(host->transition.cookie));
    } while (!host->transition.cookie);
    for (size_t i = 0; i < MAX_PM_CLIENTS; i++) {
        host->peers[i].fd = -1;
    }
    watch(fd, accept_peer, NULL, host);
    return host;
}

void venus_pm_host_close(struct VenusPMHost *host)
{
    for (size_t i = 0; i < MAX_PM_CLIENTS; i++) {
        if (host->peers[i].fd >= 0) {
            close_peer(&host->peers[i], false);
        }
    }
    host->watch(host->listener, NULL, NULL, NULL);
    close(host->listener);
    unlink(host->address.sun_path);
    free(host);
}

static void broadcast(struct VenusPMHost *host, const struct VenusPMFrame *frame)
{
    bool failed = false;

    for (size_t i = 0; i < MAX_PM_CLIENTS; i++) {
        struct VenusPMPeer *peer = &host->peers[i];

        if (peer->enrolled && !queue_frame(peer, frame)) {
            close_peer(peer, false);
            failed = true;
        }
    }
    if (failed && host->transition.phase != VENUS_PM_RUNNING) {
        host->notify(host->opaque, VENUS_PM_HOST_CLIENT_FAILED);
    }
}

bool venus_pm_host_prepare(struct VenusPMHost *host, struct VenusPMFrame *guest)
{
    size_t enrolled = 0;

    if (host->transition.phase != VENUS_PM_RUNNING) {
        return false;
    }
    for (size_t i = 0; i < MAX_PM_CLIENTS; i++) {
        struct VenusPMPeer *peer = &host->peers[i];

        if (peer->enrolled) {
            enrolled++;
        } else if (peer->fd >= 0) {
            close_peer(peer, false);
        }
    }
    if (!venus_pm_begin(&host->transition, enrolled, guest)) {
        return false;
    }
    broadcast(host, guest);
    if (!enrolled) {
        host->notify(host->opaque, VENUS_PM_HOST_CLIENTS_DRAINED);
    }
    return true;
}

bool venus_pm_host_quiesced(struct VenusPMHost *host,
                            const struct VenusPMFrame *guest)
{
    return venus_pm_guest_quiesced(&host->transition, guest);
}

bool venus_pm_host_abort(struct VenusPMHost *host)
{
    return venus_pm_abort(&host->transition);
}

bool venus_pm_host_restored_access(struct VenusPMHost *host,
                                   struct VenusPMFrame *guest)
{
    if (!venus_pm_host_restored(&host->transition, guest)) {
        return false;
    }
    broadcast(host, guest);
    return true;
}

bool venus_pm_host_ready(struct VenusPMHost *host,
                         const struct VenusPMFrame *guest)
{
    struct VenusPMFrame ready;

    if (!venus_pm_guest_ready(&host->transition, guest, &ready)) {
        return false;
    }
    broadcast(host, &ready);
    return true;
}
