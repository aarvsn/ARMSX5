#pragma once
// Single-producer/single-consumer lock-free byte ring buffer for guest audio.
// Used between the guest audio engine (producer) and the Android AAudio
// callback (consumer). Capacity is a power of two.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace armsx5::audio {

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity_pow2) : buf_(capacity_pow2), mask_(capacity_pow2 - 1) {}

    size_t write(const uint8_t* src, size_t len) {
        const size_t space = capacity() - used();
        const size_t n = len < space ? len : space;
        for (size_t i = 0; i < n; ++i)
            buf_[(write_pos_ & mask_)] = src[i], ++write_pos_;
        return n;
    }

    size_t read(uint8_t* dst, size_t len) {
        const size_t avail = used();
        const size_t n = len < avail ? len : avail;
        for (size_t i = 0; i < n; ++i)
            dst[i] = buf_[(read_pos_ & mask_)], ++read_pos_;
        return n;
    }

    size_t used() const { return write_pos_ - read_pos_; }
    size_t capacity() const { return buf_.size(); }
    void reset() { read_pos_ = write_pos_ = 0; }

    // Monotonic counters exposed for diagnostics.
    uint64_t underruns() const { return underruns_; }
    void count_underrun() { ++underruns_; }

private:
    std::vector<uint8_t> buf_;
    size_t mask_;
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
    std::atomic<uint64_t> underruns_{0};
};

} // namespace armsx5::audio
