#ifndef VIRTUALIZATION_VENUS_PM_WIRE_H
#define VIRTUALIZATION_VENUS_PM_WIRE_H

#include <stdbool.h>
#include <stdint.h>

#define VENUS_PM_FRAME_SIZE 32
#define VENUS_PM_VERSION 1
#define VENUS_PM_SOCKET_ENV "VENUS_PM_SOCKET"

/* RVPM v1 over the private VM-local stream. Every field is little-endian. */
enum VenusPMOpcode {
    VENUS_PM_HELLO = 1,
    VENUS_PM_HELLO_ACK = 2,
    VENUS_PM_PREPARE = 3,
    VENUS_PM_DRAINED = 4,
    VENUS_PM_QUIESCED = 5,
    VENUS_PM_RESUME = 6,
    VENUS_PM_ABORT = 7,
    VENUS_PM_READY = 8,
    VENUS_PM_FAILED = 9,
};

/* In-memory representation only. Use encode/decode at the socket boundary. */
struct VenusPMFrame {
    enum VenusPMOpcode opcode;
    uint64_t cookie;
    uint64_t transition;
    uint32_t status;
};

bool venus_pm_encode(uint8_t wire[VENUS_PM_FRAME_SIZE],
                     const struct VenusPMFrame *frame);
bool venus_pm_decode(struct VenusPMFrame *frame,
                     const uint8_t wire[VENUS_PM_FRAME_SIZE]);

#endif
