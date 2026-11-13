#define _GNU_SOURCE
#include "venus-pm-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PM_PORT "/dev/virtio-ports/com.texel-arts.venus-vgpu.pm"
#define RENDERER_SOCKET "/run/venus/renderer-pm.sock"
#define RENDERER_TIMEOUT_MS 8000

extern char **environ;

struct Renderer {
    int fd;
    uint64_t cookie;
    bool prepare_sent;
    bool suspend_attempted;
    bool resume_required;
};

static int64_t monotonic_ms(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static bool transfer(int fd, uint8_t *data, bool writing, int timeout_ms)
{
    int64_t deadline = monotonic_ms() + timeout_ms;
    size_t offset = 0;

    while (offset < VENUS_PM_FRAME_SIZE) {
        int64_t remaining = deadline - monotonic_ms();
        struct pollfd port = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
        int result = poll(&port, 1, timeout_ms < 0 ? -1 :
                          remaining > 0 ? (int)remaining : 0);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || !(port.revents & port.events)) {
            return false;
        }
        ssize_t count = writing ?
            write(fd, data + offset, VENUS_PM_FRAME_SIZE - offset) :
            read(fd, data + offset, VENUS_PM_FRAME_SIZE - offset);

        if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += (size_t)count;
    }
    return true;
}

static bool send_frame(int fd, const struct VenusPMFrame *frame)
{
    uint8_t bytes[VENUS_PM_FRAME_SIZE];

    return venus_pm_encode(bytes, frame) &&
           transfer(fd, bytes, true, RENDERER_TIMEOUT_MS);
}

static bool receive_frame(int fd, struct VenusPMFrame *frame, int timeout_ms)
{
    uint8_t bytes[VENUS_PM_FRAME_SIZE];

    return transfer(fd, bytes, false, timeout_ms) && venus_pm_decode(frame, bytes);
}

static bool renderer_connect(struct Renderer *renderer, uid_t expected_uid)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = RENDERER_SOCKET };
    struct ucred peer;
    socklen_t length = sizeof(peer);
    struct VenusPMFrame hello = { .opcode = VENUS_PM_HELLO };
    struct VenusPMFrame ack;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

    if (fd < 0) {
        return false;
    }
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0 ||
        length != sizeof(peer) || peer.uid != expected_uid ||
        !send_frame(fd, &hello) ||
        !receive_frame(fd, &ack, RENDERER_TIMEOUT_MS) ||
        ack.opcode != VENUS_PM_HELLO_ACK) {
        close(fd);
        return false;
    }
    renderer->fd = fd;
    renderer->cookie = ack.cookie;
    return true;
}

static bool renderer_request(const struct Renderer *renderer,
                             const struct VenusPMFrame *request,
                             enum VenusPMOpcode expected)
{
    struct VenusPMFrame outgoing = *request;
    struct VenusPMFrame reply;
    int64_t deadline = monotonic_ms() + RENDERER_TIMEOUT_MS;

    outgoing.cookie = renderer->cookie;
    if (!send_frame(renderer->fd, &outgoing)) {
        return false;
    }
    for (;;) {
        int64_t remaining = deadline - monotonic_ms();

        if (remaining <= 0 || !receive_frame(renderer->fd, &reply, (int)remaining)) {
            return false;
        }
        if (reply.cookie == renderer->cookie &&
            reply.transition == request->transition &&
            (reply.opcode != VENUS_PM_DRAINED || expected == VENUS_PM_DRAINED)) {
            return reply.opcode == expected && reply.status == 0;
        }
    }
}

static bool nvidia_power(bool suspending, bool microvm)
{
    char *systemd_args[] = {
        "systemctl", "start",
        suspending ? "venus-gpu-power@suspend.service" :
                     "venus-gpu-power@resume.service",
        NULL,
    };
    char *direct_args[] = {
        "venus-gpu-power", suspending ? "suspend" : "resume", NULL,
    };
    const char *executable = microvm ? "/usr/libexec/venus-gpu-power" :
                                      "/usr/bin/systemctl";
    pid_t pid;
    pid_t finished;
    int status;

    int error = posix_spawn(&pid, executable, NULL, NULL,
                            microvm ? direct_args : systemd_args, environ);
    if (error) {
        fprintf(stderr, "Starting NVIDIA power service: %d\n", error);
        return false;
    }
    do {
        finished = waitpid(pid, &status, 0);
    } while (finished < 0 && errno == EINTR);
    return finished == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv)
{
    bool microvm = argc == 4 && strcmp(argv[1], "--microvm") == 0;
    bool nouveau = microvm && strcmp(argv[3], "nouveau") == 0;
    if (argc != 1 && (!microvm || (!nouveau && strcmp(argv[3], "nvidia")))) {
        fputs("Usage: venus-pm-bridge [--microvm /dev/vportNpn nvidia|nouveau]\n", stderr);
        return 2;
    }
    struct Renderer renderer = { .fd = -1 };
    struct VenusPMFrame request;
    struct passwd *account = getpwnam(microvm ? "venus" : "fedora");
    uint64_t vm_cookie = 0;
    uint64_t last_id = 0;
    bool active = false;
    bool quiesced = false;
    int fd = open(microvm ? argv[2] : PM_PORT,
                  O_RDWR | O_CLOEXEC | O_NONBLOCK);

    if (fd < 0 || !account) {
        fputs("Venus PM port or renderer account unavailable\n", stderr);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    while (receive_frame(fd, &request, -1)) {
        struct VenusPMFrame reply = request;

        if (request.status || !request.cookie || !request.transition ||
            (vm_cookie && request.cookie != vm_cookie)) {
            continue;
        }
        switch (request.opcode) {
        case VENUS_PM_PREPARE:
            if (nouveau) {
                /* Renderer quiescence alone does not save Nouveau VRAM. */
                reply.opcode = VENUS_PM_FAILED;
                reply.status = EOPNOTSUPP;
                break;
            }
            if (active || request.transition <= last_id) {
                continue;
            }
            active = true;
            vm_cookie = request.cookie;
            last_id = request.transition;
            if (renderer_connect(&renderer, account->pw_uid)) {
                renderer.prepare_sent = true;
                if (renderer_request(&renderer, &request, VENUS_PM_DRAINED)) {
                    renderer.suspend_attempted = true;
                    renderer.resume_required = true;
                    quiesced = nvidia_power(true, microvm);
                }
            }
            reply.opcode = quiesced ? VENUS_PM_QUIESCED : VENUS_PM_FAILED;
            reply.status = quiesced ? 0 : EIO;
            break;
        case VENUS_PM_RESUME:
        case VENUS_PM_ABORT: {
            struct VenusPMFrame renderer_request_frame = request;

            if (!active || request.transition != last_id ||
                (request.opcode == VENUS_PM_RESUME && !quiesced)) {
                continue;
            }
            if (renderer.suspend_attempted && !nvidia_power(false, microvm)) {
                reply.opcode = VENUS_PM_FAILED;
                reply.status = EIO;
                break;
            }
            renderer.suspend_attempted = false;
            if (renderer.resume_required) {
                renderer_request_frame.opcode = VENUS_PM_RESUME;
            }
            if (renderer.prepare_sent &&
                !renderer_request(&renderer, &renderer_request_frame, VENUS_PM_READY)) {
                reply.opcode = VENUS_PM_FAILED;
                reply.status = EIO;
                break;
            }
            if (renderer.fd >= 0) {
                close(renderer.fd);
            }
            renderer = (struct Renderer){ .fd = -1 };
            quiesced = false;
            active = false;
            reply.opcode = VENUS_PM_READY;
            break;
        }
        default:
            continue;
        }
        if (!send_frame(fd, &reply)) {
            break;
        }
    }
    if (active) {
        fprintf(stderr, "Venus PM control disconnected during transition %llu\n",
                (unsigned long long)last_id);
    }
    return 1;
}
