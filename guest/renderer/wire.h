#ifndef REVERSE_VENUS_WIRE_H
#define REVERSE_VENUS_WIRE_H

#include <stddef.h>
#include <stdint.h>

enum {
    RV_MAGIC = 0x33565652,
    RV_HELLO = 1,
    RV_SUBMIT,
    RV_READBACK,
    RV_CLOSE,
    RV_CONTROL,
    RV_STREAM,
    RV_WAIT_GPU,
    RV_CONTEXT_OPEN,
    RV_CONTEXT_CLOSE,
    RV_CONTEXT_COUNT = 16,
    RV_REPLY_RESOURCE = 1,
    RV_READBACK_RESOURCE = 2,
    RV_LIMIT = 1024 * 1024,
    RV_APERTURE_SIZE = 4 * 1024 * 1024,
    RV_APERTURE_LARGE = 32 * 1024 * 1024,
    RV_RING_SIZE = 8,
    RV_GPU_WAIT_CAPABILITY = 0x57414954,
    RV_ORDERED_STREAM_CAPABILITY = 0x4f524452,
    RV_GPU_WAIT_SLOTS = 16,
    RV_GPU_WAIT_BYTES = 144,
    RV_COMMAND_LIMIT = 64 * 1024,
    RV_REPLY_LIMIT = 256 * 1024,
};

struct rv_request {
    uint32_t operation;
    uint32_t command_bytes;
    uint32_t reply_bytes;
    uint32_t context_id;
    uint64_t memory_id;
    uint64_t memory_size;
    uint64_t offset;
};

struct rv_submission {
    struct rv_request request;
    unsigned char command[RV_COMMAND_LIMIT];
};

struct rv_completion {
    int32_t status;
    uint32_t bytes;
    unsigned char reply[RV_REPLY_LIMIT];
};

struct rv_gpu_wait_slot {
    uint64_t requested;
    uint64_t completed;
    uint32_t context;
    int32_t status;
    unsigned char command[RV_GPU_WAIT_BYTES];
};

/* Tickets are one-based. A slot cannot be reused until the host retires its
 * completion. The only data on the notification channel after setup is a kick. */
struct rv_aperture {
    _Alignas(64) uint32_t ready;
    uint32_t guest_peer;
    uint32_t client_peer;
    uint64_t client_epoch;
    uint64_t accepted_epoch;
    uint32_t gpu_wait_capability;
    uint32_t ordered_stream_capability;
    _Alignas(64) uint64_t submitted;
    _Alignas(64) uint64_t completed;
    _Alignas(64) struct rv_submission requests[RV_RING_SIZE];
    _Alignas(64) struct rv_completion replies[RV_RING_SIZE];
    struct rv_gpu_wait_slot gpu_waits[RV_GPU_WAIT_SLOTS];
};

static inline size_t rv_slot(uint64_t sequence)
{
    return (sequence - 1) % RV_RING_SIZE;
}

_Static_assert(sizeof(struct rv_request) == 40, "wire request layout");
_Static_assert(offsetof(struct rv_aperture, ordered_stream_capability) == 36, "wire capability layout");
_Static_assert(offsetof(struct rv_aperture, requests) == 192, "wire aperture layout");
_Static_assert((sizeof(struct rv_aperture) + 4095) / 4096 ==
               (offsetof(struct rv_aperture, gpu_waits) + 4095) / 4096,
               "fence slots must fit before the existing lease area");
_Static_assert(sizeof(struct rv_aperture) <= RV_APERTURE_SIZE, "wire aperture capacity");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "shared index must be lock free");

#endif
