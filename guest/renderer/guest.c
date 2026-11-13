#define _GNU_SOURCE
#include "wire.h"
#include "rv_control.h"
#include "rv_guest_pool_uapi.h"
#include "renderer-pm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <virglrenderer.h>

/* Venus capset and reply-stream opcode from the pinned protocol. */
enum {
    VENUS_CAPSET = 4,
    VENUS_SEEK_REPLY_OPCODE = 179,
    VENUS_SEEK_REPLY_BYTES = 16,
};

#define VENUS_IVSHMEM_PEER _IOR('V', 0, uint32_t)
#define VENUS_IVSHMEM_SLICE _IOW('V', 1, struct rv_ivshmem_slice)
#define VENUS_IVSHMEM_SIZE _IOR('V', 2, uint64_t)
#define RV_VRAM_ID_FIRST UINT32_C(0x40000000)
#define RV_VRAM_ID_END   UINT32_C(0x80000000)
#define RV_VRAM_POOL_MAX (1536ULL * 1024 * 1024)
#define RV_VRAM_DEVICE "/dev/rv-guest-pool"

struct rv_ivshmem_slice { uint64_t offset, length; };
struct guest_lease { int used; struct rv_lease_desc desc; };
struct pending_stream { int used; uint32_t ring_idx, sync_count; uint64_t fence; struct rv_stream_sync syncs[RV_CTL_MAX_WAIT_CONDITIONS]; };
struct guest_server {
    struct rv_aperture *aperture;
    uint64_t aperture_size;
    int ivfd;
    int poolfd;
    uint64_t generation;
    uint64_t session_serial;
    uint32_t context_id;
    uint32_t reply_resource;
    uint64_t next_fence;
    _Atomic uint64_t retired_fence;
    void *reply;
    uint64_t reply_size;
    int stream_error;
    uint64_t last_gpu_fence;
    struct rv_control *control;
    struct rv_stream_desc current_stream;
    uint64_t next_gpu_fence[RV_CTL_MAX_QUEUES];
    struct pending_stream pending[RV_CTL_MAX_QUEUES][RV_RING_SIZE];
    pthread_mutex_t pending_mutex;
    struct guest_lease leases[RV_CTL_MAX_LEASES];
};

_Static_assert(sizeof(struct rv_gpu_wait) == RV_GPU_WAIT_BYTES, "fence wait wire size");
_Static_assert(sizeof(struct rv_ctl_request) == 48, "control request ABI");
_Static_assert(sizeof(struct rv_ctl_reply) == 200, "control reply ABI");

static volatile sig_atomic_t stopping;
static _Atomic(struct guest_server *) active_server;

static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void no_gl_fence(void *cookie, uint32_t fence)
{
    (void)cookie;
    (void)fence;
}

static void retire_cpu_fence(void *cookie, uint32_t context, uint32_t ring, uint64_t fence)
{
    (void)cookie;
    struct guest_server *servers =
        atomic_load_explicit(&active_server, memory_order_acquire);
    if (!servers || !context || context > RV_CONTEXT_COUNT ||
        ring >= RV_CTL_MAX_QUEUES)
        return;
    struct guest_server *s = &servers[context - 1];
    if (!ring) {
        atomic_store_explicit(&s->retired_fence, fence, memory_order_release);
        return;
    }
    pthread_mutex_lock(&s->pending_mutex);
    if (!s->control) {
        pthread_mutex_unlock(&s->pending_mutex);
        return;
    }
    for (size_t i = 0; i < RV_RING_SIZE; i++) {
        struct pending_stream *p = &s->pending[ring][i];
        if (!p->used || p->fence > fence)
            continue;
        for (uint32_t j = 0; j < p->sync_count; j++)
            rv_control_signal_gpu(s->control, p->syncs[j].sync_id, p->syncs[j].value);
        memset(p, 0, sizeof(*p));
    }
    pthread_mutex_unlock(&s->pending_mutex);
}

static int wait_for_decode(struct guest_server *s)
{
    const uint64_t fence = ++s->next_fence;
    int result = virgl_renderer_context_create_fence(s->context_id, 0, 0, fence);
    if (result)
        return result;
    struct pollfd event = { .fd = virgl_renderer_context_get_poll_fd(s->context_id), .events = POLLIN };
    for (;;) {
        virgl_renderer_context_poll(s->context_id);
        if (atomic_load_explicit(&s->retired_fence, memory_order_acquire) == fence)
            return 0;
        if (event.fd < 0)
            return EIO;
        int ready = poll(&event, 1, 20000);
        if (ready <= 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return ready == 0 ? ETIMEDOUT : EIO;
    }
}

static int create_blob(struct guest_server *s, uint32_t id, uint64_t memory, uint64_t size)
{
    const struct virgl_renderer_resource_create_blob_args args = {
        .res_handle = id,
        .ctx_id = s->context_id,
        .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
        .blob_flags = VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE,
        .blob_id = memory,
        .size = size,
    };
    return virgl_renderer_resource_create_blob(&args);
}

static struct guest_lease *find_guest_lease(struct guest_server *s, uint64_t id)
{
    for (size_t i = 0; i < RV_CTL_MAX_LEASES; ++i)
        if (s->leases[i].used && s->leases[i].desc.resource_id == id)
            return &s->leases[i];
    return NULL;
}

static int is_vram_resource_id(uint64_t id)
{
    return id >= RV_VRAM_ID_FIRST && id < RV_VRAM_ID_END;
}

static int query_vram_pool(struct guest_server *s, struct rv_pool_query *query)
{
    memset(query, 0, sizeof(*query));
    if (s->poolfd < 0 || ioctl(s->poolfd, RV_POOL_IOC_QUERY, query) < 0)
        return -1;
    if (query->version != RV_POOL_ABI_VERSION ||
        query->alignment != RV_POOL_ALIGN ||
        query->pool_size <= query->alignment || query->pool_size > RV_VRAM_POOL_MAX)
        return -1;
    return 0;
}

static int backend_query_caps(void *userdata, uint32_t caps[RV_CTL_CAP_WORDS], uint32_t *count)
{
    (void)userdata;
    uint32_t version = 0, bytes = 0;
    virgl_renderer_get_cap_set(VENUS_CAPSET, &version, &bytes);
    if (bytes != RV_CTL_CAP_WORDS * sizeof(uint32_t))
        return EPROTO;
    memset(caps, 0, RV_CTL_CAP_WORDS * sizeof(*caps));
    virgl_renderer_fill_caps(VENUS_CAPSET, version, caps);
    *count = RV_CTL_CAP_WORDS;
    return 0;
}

static int backend_import_lease(void *userdata, const struct rv_lease_desc *d)
{
    struct guest_server *s = userdata;
    if (is_vram_resource_id(d->resource_id))
        return EINVAL;
    if (d->offset > s->aperture_size || d->length > s->aperture_size - d->offset ||
        d->resource_id > UINT32_MAX)
        return EINVAL;
    struct guest_lease *slot = NULL;
    for (size_t i = 0; i < RV_CTL_MAX_LEASES; ++i)
        if (!s->leases[i].used) { slot = &s->leases[i]; break; }
    if (!slot)
        return ENOSPC;

    struct rv_ivshmem_slice slice = { d->offset, d->length };
    int fd = ioctl(s->ivfd, VENUS_IVSHMEM_SLICE, &slice);
    if (fd < 0)
        return errno ? errno : EIO;
    const struct virgl_renderer_resource_import_blob_args args = {
        .res_handle = (uint32_t)d->resource_id,
        .blob_mem = VIRGL_RENDERER_BLOB_MEM_GUEST_VRAM,
        .fd_type = VIRGL_RENDERER_BLOB_FD_TYPE_SHM,
        .fd = fd,
        .size = d->length,
    };
    int error = virgl_renderer_resource_import_blob(&args);
    if (error) {
        if (fcntl(fd, F_GETFD) >= 0)
            close(fd);
        return error;
    }
    virgl_renderer_ctx_attach_resource(s->context_id, (int)d->resource_id);
    slot->used = 1;
    slot->desc = *d;
    return 0;
}

static int backend_release_lease(void *userdata, const struct rv_lease_desc *d)
{
    struct guest_server *s = userdata;
    if (is_vram_resource_id(d->resource_id))
        return EINVAL;
    struct guest_lease *lease = find_guest_lease(s, d->resource_id);
    if (!lease)
        return ENOENT;
    virgl_renderer_ctx_detach_resource(s->context_id, (int)d->resource_id);
    virgl_renderer_resource_unref((uint32_t)d->resource_id);
    memset(lease, 0, sizeof(*lease));
    return 0;
}

static int backend_submit_stream(void *userdata, uint64_t ticket, uint64_t id,
                                 uint64_t offset, uint64_t length)
{
    struct guest_server *s = userdata;
    struct guest_lease *lease = find_guest_lease(s, id);
    const struct rv_stream_desc *batch = &s->current_stream;
    if (!lease || lease->desc.resource_class != RV_LEASE_COMMAND || !length ||
        (length & 3) || offset > lease->desc.length || length > lease->desc.length - offset)
        return EINVAL;
    if (batch->generation != s->generation || batch->lease_id != id ||
        batch->offset != offset || batch->length != length ||
        batch->ring_idx >= RV_CTL_MAX_QUEUES ||
        batch->sync_count > RV_CTL_MAX_WAIT_CONDITIONS)
        return EINVAL;
    const unsigned char *command = (const unsigned char *)s->aperture +
                                   lease->desc.offset + offset;
    int error = virgl_renderer_submit_cmd((void *)command, s->context_id, length / 4);
    if (!error)
        error = wait_for_decode(s);
    uint64_t fence = 0;
    struct pending_stream *pending = NULL;
    if (!error && batch->sync_count && !batch->ring_idx) {
        for (uint32_t i = 0; i < batch->sync_count; ++i)
            rv_control_signal_gpu(s->control, batch->syncs[i].sync_id, batch->syncs[i].value);
    }
    if (!error && batch->sync_count && batch->ring_idx) {
        pthread_mutex_lock(&s->pending_mutex);
        for (size_t i = 0; i < RV_RING_SIZE; i++)
            if (!s->pending[batch->ring_idx][i].used) { pending = &s->pending[batch->ring_idx][i]; break; }
        if (!pending)
            error = ENOSPC;
        else {
            fence = ++s->next_gpu_fence[batch->ring_idx];
            *pending = (struct pending_stream){ .used = 1, .ring_idx = batch->ring_idx,
                                                .sync_count = batch->sync_count, .fence = fence };
            memcpy(pending->syncs, batch->syncs,
                   batch->sync_count * sizeof(batch->syncs[0]));
        }
        pthread_mutex_unlock(&s->pending_mutex);
    }
    if (!error && fence)
        error = virgl_renderer_context_create_fence(s->context_id, 0, batch->ring_idx, fence);
    if (error && pending) {
        pthread_mutex_lock(&s->pending_mutex);
        if (pending->used && pending->fence == fence)
            memset(pending, 0, sizeof(*pending));
        pthread_mutex_unlock(&s->pending_mutex);
    }
    s->last_gpu_fence = fence;
    s->stream_error = error;
    (void)ticket;
    return error;
}

static struct rv_control *new_control(struct guest_server *s, uint64_t generation)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || ((unsigned long)page & ((unsigned long)page - 1)))
        return NULL;
    const struct rv_backend backend = {
        .userdata = s,
        .query_caps = backend_query_caps,
        .lease_import = backend_import_lease,
        .lease_release = backend_release_lease,
        .submit_stream = backend_submit_stream,
    };
    s->generation = generation;
    return rv_control_create(generation, (uint64_t)page, &backend);
}

static void send_reply(struct rv_aperture *aperture, uint64_t sequence,
                       int status, const void *data, uint32_t bytes)
{
    struct rv_completion *entry = &aperture->replies[rv_slot(sequence)];
    if (!status && bytes)
        memcpy(entry->reply, data, bytes);
    entry->status = status;
    entry->bytes = status ? 0 : bytes;
}

static void close_guest_session(struct guest_server *s);

static int open_session(struct guest_server *s)
{
    int error = virgl_renderer_context_create_with_flags(s->context_id,
                                                         VENUS_CAPSET, 13,
                                                         "reverse-venus");
    if (error)
        return error;

    error = create_blob(s, s->reply_resource, 0, RV_LIMIT);
    if (!error) {
        error = virgl_renderer_resource_map(s->reply_resource, &s->reply,
                                            &s->reply_size);
        if (error)
            virgl_renderer_resource_unref(s->reply_resource);
    }
    if (error)
        virgl_renderer_context_destroy(s->context_id);
    return error;
}

static int open_guest_session(struct guest_server *s, uint64_t epoch)
{
    if (s->control)
        return 0;
    if (s->reply)
        close_guest_session(s);
    int error = open_session(s);
    if (error)
        return error;
    struct rv_control *control = new_control(s, epoch);
    if (!control) {
        close_guest_session(s);
        return ENOMEM;
    }
    pthread_mutex_lock(&s->pending_mutex);
    s->control = control;
    ++s->session_serial;
    pthread_mutex_unlock(&s->pending_mutex);
    return 0;
}

static void handle_hello(struct guest_server *s, struct rv_aperture *aperture,
                         uint64_t sequence)
{
    close_guest_session(s);
    int error = open_session(s);
    uint32_t capset[RV_CTL_CAP_WORDS] = {0};
    if (!error) {
        uint32_t count = 0;
        error = backend_query_caps(s, capset, &count);
        if (error)
            close_guest_session(s);
    }
    fprintf(stderr, "Venus context opened at sequence=%llu result=%d\n",
            (unsigned long long)sequence, error);
    send_reply(aperture, sequence, error, capset, 4 * sizeof(uint32_t));
}

static void handle_submit(struct guest_server *s, struct rv_aperture *aperture, uint64_t sequence,
                          const struct rv_request *request, void *command, void *reply)
{
    memcpy(command, aperture->requests[rv_slot(sequence)].command, request->command_bytes);
    memset(reply, 0, request->reply_bytes);
    int error = virgl_renderer_submit_cmd(command, s->context_id, request->command_bytes / 4);
    if (!error)
        error = wait_for_decode(s);
    uint32_t opcode;
    memcpy(&opcode, command, sizeof(opcode));
    if (opcode == VENUS_SEEK_REPLY_OPCODE &&
        request->command_bytes >= VENUS_SEEK_REPLY_BYTES + sizeof(opcode))
        memcpy(&opcode, (unsigned char *)command + VENUS_SEEK_REPLY_BYTES, sizeof(opcode));
    fprintf(stderr, "Venus sequence=%llu opcode=%u bytes=%u result=%d\n",
            (unsigned long long)sequence, opcode, request->command_bytes, error);
    send_reply(aperture, sequence, error, reply, request->reply_bytes);
}

static void handle_readback(struct guest_server *s, struct rv_aperture *aperture, uint64_t sequence,
                            const struct rv_request *request)
{
    void *mapped = NULL;
    uint64_t size = 0;
    int error = create_blob(s, RV_READBACK_RESOURCE, request->memory_id, request->memory_size);
    const bool blob_created = !error;
    if (blob_created)
        error = virgl_renderer_resource_map(RV_READBACK_RESOURCE, &mapped, &size);
    if (!error && size < request->offset + request->reply_bytes)
        error = EINVAL;
    fprintf(stderr, "Venus readback sequence=%llu memory=%llu bytes=%u result=%d\n",
            (unsigned long long)sequence, (unsigned long long)request->memory_id,
            request->reply_bytes, error);
    send_reply(aperture, sequence, error,
               mapped ? (unsigned char *)mapped + request->offset : NULL,
               request->reply_bytes);
    if (mapped)
        virgl_renderer_resource_unmap(RV_READBACK_RESOURCE);
    if (blob_created)
        virgl_renderer_resource_unref(RV_READBACK_RESOURCE);
}

static void close_guest_session(struct guest_server *s)
{
    if (s->reply) {
        virgl_renderer_context_destroy(s->context_id);
        virgl_renderer_resource_unmap(s->reply_resource);
        virgl_renderer_resource_unref(s->reply_resource);
        s->reply = NULL;
        s->reply_size = 0;
    }
    for (size_t i = 0; i < RV_CTL_MAX_LEASES; ++i) {
        if (!s->leases[i].used)
            continue;
        virgl_renderer_resource_unref((uint32_t)s->leases[i].desc.resource_id);
        memset(&s->leases[i], 0, sizeof(s->leases[i]));
    }
    pthread_mutex_lock(&s->pending_mutex);
    if (s->control) {
        rv_control_renderer_died(s->control);
        rv_control_destroy(s->control);
        s->control = NULL;
    }
    memset(s->pending, 0, sizeof(s->pending));
    pthread_mutex_unlock(&s->pending_mutex);
}

static enum rv_ctl_status handle_vram_control(struct guest_server *s,
                                               const struct rv_ctl_request *q,
                                               struct rv_ctl_reply *r)
{
    const struct rv_lease_desc *d = &q->u.lease;
    if (!s->control)
        return RV_CTL_STALE_SESSION;
    if (q->reserved || d->reserved)
        return RV_CTL_INVALID;
    if (d->generation != s->generation)
        return RV_CTL_STALE_SESSION;

    if (q->opcode == RV_CTL_QUERY_VRAM) {
        struct rv_pool_query query;
        if (query_vram_pool(s, &query))
            return RV_CTL_BACKEND_ERROR;
        r->ticket = query.alignment;
        r->resource_size = query.pool_size;
        r->generation = s->generation;
        memcpy(r->caps, query.pool_id, RV_POOL_ID_BYTES);
        r->cap_count = RV_POOL_ID_BYTES / sizeof(uint32_t);
        return RV_CTL_OK;
    }

    if (q->opcode == RV_CTL_ALLOC_VRAM) {
        if (!is_vram_resource_id(d->resource_id) ||
            d->resource_class != RV_LEASE_APPLICATION ||
            d->offset != RV_POOL_ALIGN || !d->length ||
            d->length > RV_VRAM_POOL_MAX ||
            (d->length & (RV_POOL_ALIGN - 1)) || find_guest_lease(s, d->resource_id))
            return RV_CTL_INVALID;
        struct rv_pool_query query;
        if (query_vram_pool(s, &query))
            return RV_CTL_BACKEND_ERROR;
        struct guest_lease *slot = NULL;
        for (size_t i = 0; i < RV_CTL_MAX_LEASES; ++i)
            if (!s->leases[i].used) { slot = &s->leases[i]; break; }
        if (!slot) {
            fprintf(stderr, "Venus GPU lease table full for %llu bytes\n", (unsigned long long)d->length);
            return RV_CTL_OUT_OF_SPACE;
        }
        struct rv_pool_alloc alloc = { .bytes = d->length, .map_fd = -1 };
        if (ioctl(s->poolfd, RV_POOL_IOC_ALLOC, &alloc) < 0) {
            int error = errno;
            fprintf(stderr, "Venus GPU pool request for %llu bytes failed: %s\n",
                    (unsigned long long)d->length, strerror(error));
            return error == ENOSPC ? RV_CTL_OUT_OF_SPACE : RV_CTL_BACKEND_ERROR;
        }
        if (alloc.map_fd < 0 || !alloc.length ||
            alloc.length != d->length || (alloc.offset & (RV_POOL_ALIGN - 1)) ||
            (alloc.length & (RV_POOL_ALIGN - 1)) ||
            alloc.offset > query.pool_size ||
            alloc.length > query.pool_size - alloc.offset) {
            if (alloc.map_fd >= 0)
                close(alloc.map_fd);
            return RV_CTL_BACKEND_ERROR;
        }
        const struct virgl_renderer_resource_import_blob_args args = {
            .res_handle = (uint32_t)d->resource_id,
            .blob_mem = VIRGL_RENDERER_BLOB_MEM_GUEST_VRAM,
            .fd_type = VIRGL_RENDERER_BLOB_FD_TYPE_SHM,
            .fd = alloc.map_fd,
            .size = alloc.length,
        };
        int error = virgl_renderer_resource_import_blob(&args);
        if (error) {
            if (fcntl(alloc.map_fd, F_GETFD) >= 0)
                close(alloc.map_fd);
            return RV_CTL_BACKEND_ERROR;
        }
        virgl_renderer_ctx_attach_resource(s->context_id, (int)d->resource_id);
        slot->used = 1;
        slot->desc = (struct rv_lease_desc){
            .generation = s->generation,
            .resource_id = d->resource_id,
            .offset = alloc.offset,
            .length = alloc.length,
            .resource_class = RV_LEASE_APPLICATION,
        };
        r->ticket = alloc.offset;
        r->resource_id = d->resource_id;
        r->resource_size = alloc.length;
        r->generation = s->generation;
        return RV_CTL_OK;
    }

    if (q->opcode == RV_CTL_RELEASE_VRAM) {
        if (!is_vram_resource_id(d->resource_id) ||
            d->resource_class != RV_LEASE_APPLICATION || !d->length)
            return RV_CTL_INVALID;
        struct guest_lease *lease = find_guest_lease(s, d->resource_id);
        if (!lease)
            return RV_CTL_NOT_FOUND;
        if (lease->desc.generation != d->generation ||
            lease->desc.offset != d->offset || lease->desc.length != d->length ||
            lease->desc.resource_class != d->resource_class)
            return RV_CTL_INVALID;
        virgl_renderer_ctx_detach_resource(s->context_id, (int)d->resource_id);
        virgl_renderer_resource_unref((uint32_t)d->resource_id);
        memset(lease, 0, sizeof(*lease));
        return RV_CTL_OK;
    }
    return RV_CTL_INVALID;
}

static void handle_control(struct guest_server *s, struct rv_aperture *aperture,
                           uint64_t sequence, const struct rv_request *request)
{
    struct rv_ctl_reply reply = {0};
    int transport_error = 0;
    struct rv_ctl_request control;
    if (request->command_bytes != sizeof(control) ||
        request->reply_bytes != sizeof(reply) || !s->control) {
        transport_error = EINVAL;
    } else {
        memcpy(&control, aperture->requests[rv_slot(sequence)].command, sizeof(control));
        s->stream_error = 0;
        enum rv_ctl_status status = (control.opcode == RV_CTL_QUERY_VRAM ||
                                     control.opcode == RV_CTL_ALLOC_VRAM ||
                                     control.opcode == RV_CTL_RELEASE_VRAM)
            ? handle_vram_control(s, &control, &reply)
            : rv_control_dispatch(s->control, &control, &reply);
        if (status == RV_CTL_OK && control.opcode == RV_CTL_SUBMIT_STREAM && reply.ticket)
            rv_control_complete_cpu(s->control, reply.ticket,
                                    s->stream_error ? RV_CTL_BACKEND_ERROR : RV_CTL_OK);
    }
    send_reply(aperture, sequence, transport_error,
               transport_error ? NULL : &reply, transport_error ? 0 : sizeof(reply));
}

static void handle_stream(struct guest_server *s, struct rv_aperture *aperture,
                          uint64_t sequence, const struct rv_request *request)
{
    struct rv_ctl_reply reply = {0};
    if (request->command_bytes != sizeof(struct rv_stream_desc) ||
        request->reply_bytes != sizeof(reply) || !s->control) {
        send_reply(aperture, sequence, EINVAL, NULL, 0);
        return;
    }
    struct rv_stream_desc batch;
    memcpy(&batch, aperture->requests[rv_slot(sequence)].command, sizeof(batch));
    if (batch.reserved || batch.sync_count > RV_CTL_MAX_WAIT_CONDITIONS) {
        send_reply(aperture, sequence, EINVAL, NULL, 0);
        return;
    }
    for (uint32_t i = 0; i < batch.sync_count; i++) {
        if (batch.syncs[i].reserved) {
            send_reply(aperture, sequence, EINVAL, NULL, 0);
            return;
        }
        for (uint32_t j = 0; j < i; j++)
            if (batch.syncs[i].sync_id == batch.syncs[j].sync_id) {
                send_reply(aperture, sequence, EINVAL, NULL, 0);
                return;
            }
    }
    s->current_stream = batch;
    struct rv_ctl_request submit = { .opcode = RV_CTL_SUBMIT_STREAM };
    submit.u.submit = (struct rv_submit_desc){ batch.generation, batch.lease_id,
                                               batch.offset, batch.length };
    s->stream_error = 0;
    enum rv_ctl_status status = rv_control_dispatch(s->control, &submit, &reply);
    if (status == RV_CTL_OK && reply.ticket)
        rv_control_complete_cpu(s->control, reply.ticket,
                                s->stream_error ? RV_CTL_BACKEND_ERROR : RV_CTL_OK);
    if (status == RV_CTL_OK) {
        reply.resource_id = s->last_gpu_fence;
        reply.resource_size = batch.ring_idx;
    }
    memset(&s->current_stream, 0, sizeof(s->current_stream));
    send_reply(aperture, sequence, 0, &reply, sizeof(reply));
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int32_t wait_gpu_fences(struct guest_server *s, const struct rv_gpu_wait *wait)
{
    const uint64_t deadline = monotonic_ms() + wait->timeout_ms;
    for (;;) {
        virgl_renderer_context_poll(s->context_id);
        const int32_t status = rv_control_wait_gpu(s->control, wait->conditions, wait->count,
                                                   !wait->wait_any, 0);
        if (status != RV_CTL_TIMEOUT)
            return status;
        const uint64_t now = monotonic_ms();
        if (now >= deadline)
            return RV_CTL_TIMEOUT;
        const uint64_t remaining = deadline - now;
        const int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd event = { .fd = virgl_renderer_context_get_poll_fd(s->context_id), .events = POLLIN };
        if (event.fd < 0)
            return RV_CTL_DEVICE_LOST;
        const int ready = poll(&event, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || event.revents & (POLLERR | POLLHUP | POLLNVAL))
            return RV_CTL_DEVICE_LOST;
    }
}

static void handle_gpu_wait(struct guest_server *s, struct rv_aperture *aperture,
                           uint64_t sequence, const struct rv_request *request)
{
    int32_t status = RV_CTL_INVALID;
    if (request->command_bytes == sizeof(struct rv_gpu_wait) && request->reply_bytes == sizeof(status)) {
        struct rv_gpu_wait wait;
        memcpy(&wait, aperture->requests[rv_slot(sequence)].command, sizeof(wait));
        if (wait.count && wait.count <= RV_CTL_MAX_WAIT_CONDITIONS &&
            wait.wait_any <= 1 && !wait.reserved && s->control)
            status = wait_gpu_fences(s, &wait);
    }
    send_reply(aperture, sequence, 0, &status, sizeof(status));
}

static void process_request(struct rv_aperture *aperture, uint64_t sequence,
                            uint64_t epoch, void *command,
                            struct guest_server servers[RV_CONTEXT_COUNT])
{
    const struct rv_request request = aperture->requests[rv_slot(sequence)].request;
    if (request.context_id >= RV_CONTEXT_COUNT ||
        request.command_bytes > RV_COMMAND_LIMIT ||
        request.reply_bytes > RV_REPLY_LIMIT || (request.command_bytes & 3)) {
        send_reply(aperture, sequence, EINVAL, NULL, 0);
        return;
    }

    struct guest_server *s = &servers[request.context_id];
    const bool empty = !request.command_bytes && !request.reply_bytes;
    if (request.operation == RV_CONTEXT_OPEN && empty) {
        send_reply(aperture, sequence, open_guest_session(s, epoch), NULL, 0);
    } else if (request.operation == RV_CONTEXT_CLOSE && empty) {
        close_guest_session(s);
        send_reply(aperture, sequence, 0, NULL, 0);
    } else if (request.operation == RV_CONTROL) {
        handle_control(s, aperture, sequence, &request);
    } else if (request.operation == RV_STREAM) {
        handle_stream(s, aperture, sequence, &request);
    } else if (request.operation == RV_WAIT_GPU) {
        handle_gpu_wait(s, aperture, sequence, &request);
    } else if (!request.context_id && request.operation == RV_HELLO &&
               !request.command_bytes && request.reply_bytes == sizeof(uint32_t) * 4) {
        handle_hello(s, aperture, sequence);
    } else if (!request.context_id && request.operation == RV_SUBMIT &&
               s->reply && request.command_bytes && request.reply_bytes <= s->reply_size) {
        handle_submit(s, aperture, sequence, &request, command, s->reply);
    } else if (!request.context_id && request.operation == RV_READBACK &&
               s->reply && !request.command_bytes && request.memory_id &&
               request.memory_size <= RV_LIMIT && request.offset <= request.memory_size &&
               request.reply_bytes <= request.memory_size - request.offset) {
        handle_readback(s, aperture, sequence, &request);
    } else if (!request.context_id && request.operation == RV_CLOSE && empty) {
        close_guest_session(s);
        send_reply(aperture, sequence, 0, NULL, 0);
    } else {
        send_reply(aperture, sequence, EINVAL, NULL, 0);
    }
}

struct pending_gpu_wait {
    uint64_t request;
    uint64_t deadline;
    uint64_t session_serial;
};

static bool service_gpu_waits(struct rv_aperture *aperture,
                              struct guest_server servers[RV_CONTEXT_COUNT],
                              struct pending_gpu_wait pending[RV_GPU_WAIT_SLOTS],
                              struct pollfd events[RV_CONTEXT_COUNT + 1],
                              int *timeout)
{
    const uint64_t now = monotonic_ms();
    bool notified = false;
    bool checked[RV_CONTEXT_COUNT] = {0};
    *timeout = -1;
    int failed_fd[RV_CONTEXT_COUNT];
    for (uint32_t i = 0; i < RV_CONTEXT_COUNT; ++i) {
        struct pollfd *event = &events[i + 1];
        failed_fd[i] = event->revents & (POLLERR | POLLHUP | POLLNVAL) ? event->fd : -1;
        *event = (struct pollfd){ .fd = -1, .events = POLLIN };
    }

    for (unsigned i = 0; i < RV_GPU_WAIT_SLOTS; ++i) {
        struct rv_gpu_wait_slot *slot = &aperture->gpu_waits[i];
        const uint64_t sequence = __atomic_load_n(&slot->requested, __ATOMIC_ACQUIRE);
        if (sequence == __atomic_load_n(&slot->completed, __ATOMIC_RELAXED))
            continue;

        struct rv_gpu_wait wait;
        memcpy(&wait, slot->command, sizeof(wait));
        const uint32_t context = slot->context;
        int32_t status = RV_CTL_INVALID;
        if (context < RV_CONTEXT_COUNT && wait.count &&
            wait.count <= RV_CTL_MAX_WAIT_CONDITIONS && wait.wait_any <= 1 && !wait.reserved) {
            struct guest_server *s = &servers[context];
            if (pending[i].request != sequence) {
                pending[i] = (struct pending_gpu_wait){
                    .request = sequence,
                    .deadline = wait.timeout_ms == UINT32_MAX ? UINT64_MAX : now + wait.timeout_ms,
                    .session_serial = s->session_serial,
                };
            }
            if (!s->control || pending[i].session_serial != s->session_serial) {
                status = RV_CTL_DEVICE_LOST;
            } else {
                struct pollfd *event = &events[context + 1];
                if (!checked[context]) {
                    checked[context] = true;
                    virgl_renderer_context_poll(s->context_id);
                    event->fd = virgl_renderer_context_get_poll_fd(s->context_id);
                }
                status = rv_control_wait_gpu(s->control, wait.conditions, wait.count,
                                             !wait.wait_any, 0);
                if (status == RV_CTL_TIMEOUT && now < pending[i].deadline) {
                    if (event->fd >= 0 && event->fd != failed_fd[context]) {
                        if (pending[i].deadline != UINT64_MAX) {
                            const uint64_t remaining = pending[i].deadline - now;
                            const int delay = remaining > INT_MAX ? INT_MAX : (int)remaining;
                            if (*timeout < 0 || delay < *timeout)
                                *timeout = delay;
                        }
                        continue;
                    }
                    event->fd = -1;
                    status = RV_CTL_DEVICE_LOST;
                }
            }
        }
        slot->status = status;
        __atomic_store_n(&slot->completed, sequence, __ATOMIC_RELEASE);
        notified = true;
    }
    return notified;
}

struct guest_pm {
    struct rv_aperture *aperture;
    struct guest_server *servers;
    const uint64_t *epoch;
    const uint64_t *completed;
    uint64_t transition;
    uint64_t retained_epoch;
    uint64_t retained_serial[RV_CONTEXT_COUNT];
};

static bool guest_has_pending_streams(struct guest_server *s)
{
    bool pending = false;
    pthread_mutex_lock(&s->pending_mutex);
    for (unsigned ring = 0; ring < RV_CTL_MAX_QUEUES && !pending; ring++)
        for (unsigned index = 0; index < RV_RING_SIZE && !pending; index++)
            pending = s->pending[ring][index].used;
    pthread_mutex_unlock(&s->pending_mutex);
    return pending;
}

static int guest_pm_check(struct guest_pm *pm, uint64_t transition, bool verify)
{
    const uint64_t requested =
        __atomic_load_n(&pm->aperture->client_epoch, __ATOMIC_ACQUIRE);
    const uint64_t accepted =
        __atomic_load_n(&pm->aperture->accepted_epoch, __ATOMIC_ACQUIRE);
    const uint64_t submitted =
        __atomic_load_n(&pm->aperture->submitted, __ATOMIC_ACQUIRE);
    if (*pm->epoch != requested || accepted != requested || submitted != *pm->completed)
        return verify ? EPROTO : EAGAIN;

    if (!verify && pm->transition != transition) {
        pm->transition = transition;
        pm->retained_epoch = *pm->epoch;
        for (unsigned i = 0; i < RV_CONTEXT_COUNT; i++)
            pm->retained_serial[i] = pm->servers[i].control ? pm->servers[i].session_serial : 0;
    }
    if (pm->transition != transition || pm->retained_epoch != *pm->epoch)
        return EPROTO;

    int pending = 0;
    unsigned contexts = 0;
    for (unsigned i = 0; i < RV_CONTEXT_COUNT; i++) {
        struct guest_server *s = &pm->servers[i];
        if (pm->retained_serial[i] != (s->control ? s->session_serial : 0))
            return EPROTO;
        if (!s->control)
            continue;
        contexts++;
        const int error = virgl_renderer_context_pm_check(s->context_id, transition, verify);
        if (error && error != EAGAIN)
            return error;
        virgl_renderer_context_poll(s->context_id);
        if (error || guest_has_pending_streams(s))
            pending = EAGAIN;
    }
    if (!pending)
        fprintf(stderr, "Venus PM %s transition=%llu epoch=%llu contexts=%u\n",
                verify ? "READY" : "DRAINED", (unsigned long long)transition,
                (unsigned long long)pm->retained_epoch, contexts);
    return pending;
}

static int guest_pm_drain(void *context, uint64_t transition)
{
    return guest_pm_check(context, transition, false);
}

static int guest_pm_verify(void *context, uint64_t transition)
{
    return guest_pm_check(context, transition, true);
}

static int guest_pm_abort(void *context, uint64_t transition)
{
    struct guest_pm *pm = context;
    fprintf(stderr, "Venus PM ABORT transition=%llu epoch=%llu\n",
            (unsigned long long)transition, (unsigned long long)*pm->epoch);
    return 0;
}

static bool guest_pm_accepts_work(enum rv_renderer_pm_phase phase)
{
    return phase == RV_RENDERER_RUNNING || phase == RV_RENDERER_DRAINING;
}

static int serve(struct rv_aperture *aperture, uint64_t aperture_size, int fd,
                 int poolfd, uint32_t guest)
{
    void *command = malloc(RV_COMMAND_LIMIT);
    if (!command)
        return 1;
    struct guest_server *servers = calloc(RV_CONTEXT_COUNT, sizeof(*servers));
    if (!servers) {
        free(command);
        return 1;
    }
    for (uint32_t i = 0; i < RV_CONTEXT_COUNT; i++) {
        struct guest_server *s = &servers[i];
        s->aperture = aperture;
        s->aperture_size = aperture_size;
        s->ivfd = fd;
        s->poolfd = poolfd;
        s->context_id = i + 1;
        s->reply_resource = i ? RV_READBACK_RESOURCE + i : RV_REPLY_RESOURCE;
        atomic_init(&s->retired_fence, 0);
        if (pthread_mutex_init(&s->pending_mutex, NULL)) {
            while (i)
                pthread_mutex_destroy(&servers[--i].pending_mutex);
            free(servers);
            free(command);
            return 1;
        }
    }

    uint64_t completed = 0;
    uint64_t epoch = 0;
    struct guest_pm pm_context = {
        .aperture = aperture, .servers = servers, .epoch = &epoch, .completed = &completed,
    };
    static const struct rv_renderer_pm_ops pm_ops = {
        .drain = guest_pm_drain, .verify = guest_pm_verify, .abort = guest_pm_abort,
    };
    struct rv_renderer_pm_endpoint power = {
        .listener = -1, .peer = -1, .pm = {.ops = &pm_ops, .renderer = &pm_context},
    };
    const char *enabled = getenv("VENUS_RENDERER_PM");
    if (enabled && !strcmp(enabled, "1")) {
        const char *path = getenv("VENUS_RENDERER_PM_SOCKET");
        if (rv_renderer_pm_open(&power, path ? path : RV_RENDERER_PM_PATH)) {
            perror("Venus renderer PM socket");
            for (uint32_t i = 0; i < RV_CONTEXT_COUNT; i++)
                pthread_mutex_destroy(&servers[i].pending_mutex);
            free(servers);
            free(command);
            return 1;
        }
    }

    atomic_store_explicit(&active_server, servers, memory_order_release);
    __atomic_store_n(&aperture->ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&aperture->accepted_epoch, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&aperture->client_epoch, 0, __ATOMIC_RELAXED);
    aperture->guest_peer = guest;
    aperture->gpu_wait_capability = RV_GPU_WAIT_CAPABILITY;
    aperture->ordered_stream_capability = RV_ORDERED_STREAM_CAPABILITY;
    __atomic_store_n(&aperture->ready, RV_MAGIC, __ATOMIC_RELEASE);
    fprintf(stderr, "Venus shared rings ready on IVSHMEM peer %u\n", guest);

    uint32_t peer = 0;
    int result = 0;
    struct pending_gpu_wait pending[RV_GPU_WAIT_SLOTS] = {0};
    enum { PM_EVENT_FIRST = RV_CONTEXT_COUNT + 1 };
    struct pollfd events[PM_EVENT_FIRST + 2] = {{ .fd = fd, .events = POLLIN }};
    for (uint32_t i = 1; i <= RV_CONTEXT_COUNT; ++i)
        events[i].fd = -1;
    int timeout = -1;
    while (!stopping) {
        const enum rv_renderer_pm_phase phase = power.pm.phase;
        const bool accepts_work = guest_pm_accepts_work(phase);
        events[0].fd = accepts_work ? fd : -1;
        if (!accepts_work)
            for (unsigned i = 1; i < PM_EVENT_FIRST; i++)
                events[i].fd = -1;
        int delay = accepts_work ? timeout : -1;
        if (phase == RV_RENDERER_DRAINING || phase == RV_RENDERER_VERIFYING ||
            phase == RV_RENDERER_ABORTING) {
            if (delay < 0 || delay > 4)
                delay = 4;
        }
        rv_renderer_pm_poll(&power, &events[PM_EVENT_FIRST]);
        const int ready = poll(events, PM_EVENT_FIRST + 2, delay);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            result = 1;
            break;
        }

        bool notify = false;
        const bool doorbell = events[0].revents & POLLIN;
        if (doorbell) {
            uint64_t kicks;
            const ssize_t count = read(fd, &kicks, sizeof(kicks));
            if (count < 0 && errno == EINTR)
                continue;
            if (count != sizeof(kicks)) {
                result = 1;
                break;
            }
        }
        if (doorbell || phase == RV_RENDERER_DRAINING) {
            const uint64_t requested =
                __atomic_load_n(&aperture->client_epoch, __ATOMIC_ACQUIRE);
            if (requested && requested != epoch) {
                for (uint32_t i = 0; i < RV_CONTEXT_COUNT; i++)
                    close_guest_session(&servers[i]);
                peer = aperture->client_peer;
                if (peer >= UINT16_MAX || peer == guest)
                    continue;
                epoch = requested;
                completed = 0;
                memset(pending, 0, sizeof(pending));
                memset(aperture->gpu_waits, 0, sizeof(aperture->gpu_waits));
                __atomic_store_n(&aperture->submitted, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&aperture->completed, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&aperture->accepted_epoch, epoch, __ATOMIC_RELEASE);
                (void)open_guest_session(&servers[0], epoch);
                notify = true;
            } else if (requested) {
                const uint64_t submitted =
                    __atomic_load_n(&aperture->submitted, __ATOMIC_ACQUIRE);
                if (submitted < completed || submitted - completed > RV_RING_SIZE) {
                    result = 1;
                    break;
                }
                if (submitted != completed) {
                    fprintf(stderr, "Venus MSI-X doorbell: %llu requests queued\n",
                            (unsigned long long)(submitted - completed));
                    while (completed < submitted) {
                        process_request(aperture, ++completed, epoch, command, servers);
                        __atomic_store_n(&aperture->completed, completed, __ATOMIC_RELEASE);
                    }
                    notify = true;
                }
            }
        }

        if (epoch && accepts_work &&
            service_gpu_waits(aperture, servers, pending, events, &timeout))
            notify = true;
        if (notify && write(fd, &peer, sizeof(peer)) != sizeof(peer)) {
            result = 1;
            break;
        }
        rv_renderer_pm_service(&power, &events[PM_EVENT_FIRST], monotonic_ms());
    }
    rv_renderer_pm_close(&power);
    __atomic_store_n(&aperture->ready, 0, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < RV_CONTEXT_COUNT; i++)
        close_guest_session(&servers[i]);
    atomic_store_explicit(&active_server, NULL, memory_order_release);
    for (uint32_t i = 0; i < RV_CONTEXT_COUNT; i++)
        pthread_mutex_destroy(&servers[i].pending_mutex);
    free(servers);
    free(command);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /dev/venus-ivshmem\n", argv[0]);
        return 2;
    }
    const struct sigaction action = { .sa_handler = stop };
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    int fd = open(argv[1], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }

    int poolfd = open(RV_VRAM_DEVICE, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (poolfd < 0) {
        perror(RV_VRAM_DEVICE);
        close(fd);
        return 1;
    }

    uint64_t aperture_size;
    if (ioctl(fd, VENUS_IVSHMEM_SIZE, &aperture_size) < 0 ||
        (aperture_size != RV_APERTURE_SIZE && aperture_size != RV_APERTURE_LARGE)) {
        fprintf(stderr, "Unsupported IVSHMEM aperture size\n");
        close(poolfd);
        close(fd);
        return 1;
    }
    void *aperture = mmap(NULL, aperture_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (aperture == MAP_FAILED) {
        perror("mmap shared aperture");
        close(poolfd);
        close(fd);
        return 1;
    }

    struct virgl_renderer_callbacks callbacks = {
        .version = VIRGL_RENDERER_CALLBACKS_VERSION,
        .write_fence = no_gl_fence,
        .write_context_fence = retire_cpu_fence,
    };
    if (virgl_renderer_init(NULL, VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL |
                            VIRGL_RENDERER_RENDER_SERVER | VIRGL_RENDERER_THREAD_SYNC,
                            &callbacks)) {
        fprintf(stderr, "Could not initialize the Venus renderer\n");
        munmap(aperture, aperture_size);
        close(poolfd);
        close(fd);
        return 1;
    }
    uint32_t guest;
    int result = 1;
    if (ioctl(fd, VENUS_IVSHMEM_PEER, &guest) == 0)
        result = serve(aperture, aperture_size, fd, poolfd, guest);
    else
        perror("IVSHMEM peer ID");
    virgl_renderer_cleanup(NULL);
    munmap(aperture, aperture_size);
    close(poolfd);
    close(fd);
    return result;
}
