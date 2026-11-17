#pragma once
// PM4 command-buffer decoding (AMD GPU command processor packets).
//
// Layout source: Linux amdgpu / Mesa radeonsi sid.h PACKET3 macro —
// Type-3 header: type = bits[31:30] (value 3), count = bits[29:16]
// (number of body dwords), opcode = bits[15:8]. shadPS4's video core uses
// the same packet family for PS4; PS5 firmware opcodes are a superset that
// is still being researched — unknown opcodes are recorded with their
// numeric value and never interpreted.
#include "armsx5/core/result.h"

#include <expected>
#include <cstdint>
#include <string>
#include <vector>

namespace armsx5::gpu::pm4 {

enum class Opcode : uint32_t {
    DrawIndex = 0x07,
    IndexType = 0x0A,
    NumInstances = 0x0B,
    Nop = 0x10,
    SetBase = 0x11,
    ClearState = 0x12,
    DrawIndexAuto = 0x2D,
    EventWrite = 0x46,
    SetContextReg = 0x69,
    SetShReg = 0x76,
    SetShRegOffset = 0x77,
};

struct Packet {
    uint32_t opcode = 0;      // raw opcode (8 bits)
    uint32_t count = 0;       // body dword count
    size_t header_offset = 0; // index of the header dword in the source buffer
    std::vector<uint32_t> body;
    const char* name() const; // nullptr when unknown
};

struct DecodedBuffer {
    std::vector<Packet> packets;
    size_t unknown_opcode_count = 0;
};

// Decodes a guest command buffer. Fails on non-Type-3 headers, truncated
// packets, or zero-length input (ParseError) — never guesses packet sizes.
std::expected<DecodedBuffer, Error> decode_type3(const uint32_t* dwords, size_t count);

const char* opcode_name(uint32_t opcode);

} // namespace armsx5::gpu::pm4
