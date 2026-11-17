#pragma once
// Shared SIMD lane helpers for the SSE/AVX interpreters. Internal header
// (not installed): included only by src/interpreter_sse.cpp and
// src/interpreter_avx.cpp.
//
// Float semantics: IEEE-754 with the default round-to-nearest-even mode;
// MXCSR exception flags are not modeled (documented in
// docs/architecture/cpu.md).
#include "armsx5/cpu/cpu_state.h"

#include <cmath>
#include <cstring>

namespace armsx5::cpu::vecops {

inline constexpr uint32_t kIntIndefinite32 = 0x80000000u;
inline constexpr uint64_t kIntIndefinite64 = 0x8000000000000000ULL;

// ---- 128-bit lane access (i = 0..3 for dword lanes, 0..1 for qword) ----
inline uint32_t lane_u32(const XmmValue& v, int i) {
    return i < 2 ? static_cast<uint32_t>(v.lo >> (32 * i))
                 : static_cast<uint32_t>(v.hi >> (32 * (i - 2)));
}
inline void set_lane_u32(XmmValue& v, int i, uint32_t b) {
    if (i < 2) {
        const uint64_t shift = 32 * i;
        v.lo = (v.lo & ~(0xFFFFFFFFULL << shift)) | (static_cast<uint64_t>(b) << shift);
    } else {
        const uint64_t shift = 32 * (i - 2);
        v.hi = (v.hi & ~(0xFFFFFFFFULL << shift)) | (static_cast<uint64_t>(b) << shift);
    }
}
inline float lane_f32(const XmmValue& v, int i) {
    const uint32_t b = lane_u32(v, i);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline void set_lane_f32(XmmValue& v, int i, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    set_lane_u32(v, i, b);
}
inline double lane_f64(const XmmValue& v, int i) {
    const uint64_t b = i == 0 ? v.lo : v.hi;
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
inline void set_lane_f64(XmmValue& v, int i, double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    if (i == 0) v.lo = b;
    else v.hi = b;
}

// ---- 256-bit lane access (i = 0..7 for dword lanes, 0..3 for qword) ----
inline uint32_t vec_lane_u32(const Vec256& v, int i) {
    return i < 4 ? lane_u32(v.lo, i) : lane_u32(v.hi, i - 4);
}
inline void set_vec_lane_u32(Vec256& v, int i, uint32_t b) {
    if (i < 4) set_lane_u32(v.lo, i, b);
    else set_lane_u32(v.hi, i - 4, b);
}
inline float vec_lane_f32(const Vec256& v, int i) {
    const uint32_t b = vec_lane_u32(v, i);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline void set_vec_lane_f32(Vec256& v, int i, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    set_vec_lane_u32(v, i, b);
}
inline uint64_t vec_lane_u64(const Vec256& v, int i) {
    return i == 0 ? v.lo.lo : i == 1 ? v.lo.hi : i == 2 ? v.hi.lo : v.hi.hi;
}
inline void set_vec_lane_u64(Vec256& v, int i, uint64_t b) {
    if (i == 0) v.lo.lo = b;
    else if (i == 1) v.lo.hi = b;
    else if (i == 2) v.hi.lo = b;
    else v.hi.hi = b;
}
inline double vec_lane_f64(const Vec256& v, int i) {
    const uint64_t b = vec_lane_u64(v, i);
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
inline void set_vec_lane_f64(Vec256& v, int i, double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    set_vec_lane_u64(v, i, b);
}

inline bool is_nan(float f) { return std::isnan(f); }
inline bool is_nan(double d) { return std::isnan(d); }

} // namespace armsx5::cpu::vecops
