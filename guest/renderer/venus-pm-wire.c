#include "venus-pm-wire.h"

#include <stddef.h>
#include <string.h>

static uint64_t read_le(const uint8_t *bytes, size_t size)
{
    uint64_t value = 0;
    for (size_t i = 0; i < size; i++) {
        value |= (uint64_t)bytes[i] << (i * 8);
    }
    return value;
}

static void write_le(uint8_t *bytes, size_t size, uint64_t value)
{
    for (size_t i = 0; i < size; i++) {
        bytes[i] = value >> (i * 8);
    }
}

static bool frame_valid(const struct VenusPMFrame *frame)
{
    if (frame->opcode < VENUS_PM_HELLO || frame->opcode > VENUS_PM_FAILED) {
        return false;
    }
    if (frame->opcode == VENUS_PM_HELLO) {
        return frame->cookie == 0 && frame->transition == 0 && frame->status == 0;
    }
    if (frame->opcode == VENUS_PM_HELLO_ACK) {
        return frame->cookie != 0 && frame->transition == 0 && frame->status == 0;
    }
    return frame->cookie != 0 && frame->transition != 0 &&
           (frame->opcode == VENUS_PM_FAILED ? frame->status != 0 : frame->status == 0);
}

bool venus_pm_encode(uint8_t wire[VENUS_PM_FRAME_SIZE],
                     const struct VenusPMFrame *frame)
{
    if (!frame_valid(frame)) {
        return false;
    }
    memcpy(wire, "RVPM", 4);
    write_le(wire + 4, 2, VENUS_PM_VERSION);
    write_le(wire + 6, 2, frame->opcode);
    write_le(wire + 8, 8, frame->cookie);
    write_le(wire + 16, 8, frame->transition);
    write_le(wire + 24, 4, frame->status);
    memset(wire + 28, 0, 4);
    return true;
}

bool venus_pm_decode(struct VenusPMFrame *frame,
                     const uint8_t wire[VENUS_PM_FRAME_SIZE])
{
    if (memcmp(wire, "RVPM", 4) != 0 || read_le(wire + 4, 2) != VENUS_PM_VERSION ||
        read_le(wire + 28, 4) != 0) {
        return false;
    }
    const struct VenusPMFrame decoded = {
        .opcode = read_le(wire + 6, 2),
        .cookie = read_le(wire + 8, 8),
        .transition = read_le(wire + 16, 8),
        .status = read_le(wire + 24, 4),
    };
    if (!frame_valid(&decoded)) {
        return false;
    }
    *frame = decoded;
    return true;
}
