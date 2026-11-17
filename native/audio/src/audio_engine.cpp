#include "armsx5/audio/audio_engine.h"

#if defined(__ANDROID__)
#include <aaudio/AAudio.h>
#endif

namespace armsx5::audio {

size_t resample_i16_stereo(const int16_t* src, size_t src_frames, uint32_t src_rate, int16_t* dst,
                           size_t dst_capacity_frames, uint32_t dst_rate) {
    if (!src || !dst || src_frames == 0 || dst_capacity_frames == 0 || src_rate == 0) return 0;
    const size_t out_frames = static_cast<size_t>(
        static_cast<uint64_t>(src_frames) * dst_rate / src_rate);
    const size_t n = out_frames < dst_capacity_frames ? out_frames : dst_capacity_frames;
    for (size_t i = 0; i < n; ++i) {
        const size_t s = static_cast<size_t>(static_cast<uint64_t>(i) * src_rate / dst_rate);
        if (s >= src_frames) break;
        dst[i * 2] = src[s * 2];
        dst[i * 2 + 1] = src[s * 2 + 1];
    }
    return n;
}

#if defined(__ANDROID__)
// Real AAudio output stream (NDK, API 26+).
std::unique_ptr<AudioSink> create_aaudio_sink() {
    class AAudioSink final : public AudioSink {
    public:
        std::expected<void, Error> start() override {
            AAudioStreamBuilder* builder = nullptr;
            if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr)
                return std::unexpected(Error{Status::IoError, "AAudio_createStreamBuilder failed"});
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setChannelCount(builder, kDefaultChannels);
            AAudioStreamBuilder_setSampleRate(builder, kDefaultSampleRate);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            const aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream_);
            AAudioStreamBuilder_delete(builder);
            if (r != AAUDIO_OK || stream_ == nullptr)
                return std::unexpected(Error{Status::IoError, "AAudio openStream failed"});
            if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
                AAudioStream_close(stream_);
                stream_ = nullptr;
                return std::unexpected(Error{Status::IoError, "AAudio start failed"});
            }
            return {};
        }
        void stop() override {
            if (stream_) {
                AAudioStream_requestStop(stream_);
                AAudioStream_close(stream_);
                stream_ = nullptr;
            }
        }
        size_t push_i16_stereo(const int16_t* frames, size_t frame_count) override {
            ring_.write(reinterpret_cast<const uint8_t*>(frames),
                        frame_count * sizeof(int16_t) * kDefaultChannels);
            // The AAudio callback drains the ring; a data callback is wired
            // when the guest audio engine is integrated (Phase 6).
            return frame_count;
        }
        uint64_t underruns() const override { return ring_.underruns(); }

    private:
        RingBuffer ring_{1u << 16};
        AAudioStream* stream_ = nullptr;
    };
    return std::make_unique<AAudioSink>();
}
#else
std::unique_ptr<AudioSink> create_aaudio_sink() {
    return nullptr; // AAudio is Android-only by definition
}
#endif

} // namespace armsx5::audio
