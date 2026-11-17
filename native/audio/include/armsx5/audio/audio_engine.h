#pragma once
// Guest audio pipeline.
//
// ARMSX5 outputs through AAudio on Android (NDK API 26+); AAudio is the same
// subsystem Oboe wraps, used directly to avoid an extra dependency
// (decision documented in docs/research/component-reuse-matrix.md). The
// NullAudioSink drains silently on hosts without audio hardware — it is a
// *test backend*, never presented as device audio.
#include "armsx5/core/result.h"
#include "armsx5/audio/ring_buffer.h"

#include <expected>
#include <cstdint>
#include <memory>

namespace armsx5::audio {

constexpr uint32_t kDefaultSampleRate = 48000;
constexpr uint32_t kDefaultChannels = 2;

// Stereo interleaved S16 frames pushed by the guest audio engine.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    // Starts the device stream (null sink always "runs").
    virtual std::expected<void, Error> start() = 0;
    virtual void stop() = 0;
    // Pushes frames; returns frames actually buffered.
    virtual size_t push_i16_stereo(const int16_t* frames, size_t frame_count) = 0;
    virtual uint64_t underruns() const = 0;
};

class NullAudioSink : public AudioSink {
public:
    explicit NullAudioSink(size_t ring_bytes = 1u << 16) : ring_(ring_bytes) {}
    std::expected<void, Error> start() override { return {}; }
    void stop() override {}
    size_t push_i16_stereo(const int16_t* frames, size_t frame_count) override {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(frames);
        const size_t bytes = frame_count * sizeof(int16_t) * kDefaultChannels;
        const size_t n = ring_.write(p, bytes);
        if (n < bytes) ring_.count_underrun();
        return n / (sizeof(int16_t) * kDefaultChannels);
    }
    uint64_t underruns() const override { return ring_.underruns(); }

private:
    RingBuffer ring_;
};

// Android AAudio backend. Compiled only under __ANDROID__.
std::unique_ptr<AudioSink> create_aaudio_sink();

// Simple sample-rate conversion: nearest-neighbor downsampling used only for
// diagnostics paths (not the streaming path, which uses proper SRC in
// later phases). Returns out_frame_count written.
size_t resample_i16_stereo(const int16_t* src, size_t src_frames, uint32_t src_rate,
                           int16_t* dst, size_t dst_capacity_frames, uint32_t dst_rate);

} // namespace armsx5::audio
