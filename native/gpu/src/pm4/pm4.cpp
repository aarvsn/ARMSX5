#include "armsx5/gpu/pm4/pm4.h"

namespace armsx5::gpu::pm4 {

const char* opcode_name(uint32_t opcode) {
    switch (opcode) {
    case 0x07: return "DRAW_INDEX";
    case 0x0A: return "INDEX_TYPE";
    case 0x0B: return "NUM_INSTANCES";
    case 0x10: return "NOP";
    case 0x11: return "SET_BASE";
    case 0x12: return "CLEAR_STATE";
    case 0x2D: return "DRAW_INDEX_AUTO";
    case 0x46: return "EVENT_WRITE";
    case 0x69: return "SET_CONTEXT_REG";
    case 0x76: return "SET_SH_REG";
    case 0x77: return "SET_SH_REG_OFFSET";
    default: return nullptr;
    }
}

const char* Packet::name() const { return opcode_name(opcode); }

std::expected<DecodedBuffer, Error> decode_type3(const uint32_t* dwords, size_t count) {
    if (!dwords || count == 0)
        return std::unexpected(Error{Status::InvalidArgument, "empty command buffer"});

    DecodedBuffer out;
    size_t i = 0;
    while (i < count) {
        const uint32_t header = dwords[i];
        const uint32_t type = header >> 30;
        if (type != 3) {
            char msg[96];
            snprintf(msg, sizeof(msg), "packet at %zu has type %u (only Type-3 supported)", i,
                     type);
            return std::unexpected(Error{Status::Unsupported, msg});
        }
        const uint32_t body_count = (header >> 16) & 0x3FFF;
        const uint32_t opcode = (header >> 8) & 0xFF;

        Packet p;
        p.opcode = opcode;
        p.count = body_count;
        p.header_offset = i;

        if (i + 1 + body_count > count) {
            char msg[96];
            snprintf(msg, sizeof(msg), "truncated packet at %zu: needs %u body dwords", i,
                     body_count);
            return std::unexpected(Error{Status::ParseError, msg});
        }
        p.body.assign(dwords + i + 1, dwords + i + 1 + body_count);
        if (opcode_name(opcode) == nullptr) ++out.unknown_opcode_count;

        out.packets.push_back(std::move(p));
        i += 1 + body_count;
    }
    return out;
}

} // namespace armsx5::gpu::pm4
