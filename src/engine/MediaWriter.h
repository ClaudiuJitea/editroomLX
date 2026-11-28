// The export file writer (AVAssetWriter on the Mac): MP4 (H.264 / HEVC + AAC, faststart, BT.709 tags) or
// WAV (24-bit PCM, 48 kHz stereo), through libavformat/libavcodec.
//
// Video encoders, in order, per codec: VA-API, NVENC, Vulkan Video, then software (libx264 / libx265 from
// the system FFmpeg; see IMPLEMENTATION.md §17 Q1 on licences). The first that opens is used and named in
// `encoderName()` (and in the benchmark lines).
#pragma once
#include <QString>
#include <memory>
#include <optional>

namespace er {

struct WriterSettings {
    struct Video {
        int width = 0, height = 0;
        int fpsNum = 30, fpsDen = 1;
        bool hevc = false;
        long long bitrate = 0;
        int gop = 60;
        /// An HDR project: 10-bit HEVC (Main 10) tagged BT.2020 / HLG, as an iPhone records it (H.264 can't carry
        /// HDR). Frames come as P010.
        bool hdr = false;
    };
    struct Audio {
        bool wav = false;   // PCM s24le instead of AAC
        int bitrate = 256000;
    };
    std::optional<Video> video;
    std::optional<Audio> audio;
};

class MediaWriter {
public:
    static std::unique_ptr<MediaWriter> open(const QString& path, const WriterSettings& s, QString* why = nullptr);
    ~MediaWriter();
    /// One frame, NV12 (BT.709 limited range), frames in order from 0.
    bool writeVideo(const uint8_t* nv12, long long frameIndex);
    /// One frame of an HDR export: P010 (BT.2020 limited range, HLG), the 10-bit values in the top of each 16-bit word.
    bool writeVideo10(const uint16_t* p010, long long frameIndex);
    bool isHDR() const;
    /// Interleaved stereo float at 48 kHz, in order.
    bool writeAudio(const float* stereo, int frames);
    bool finish();
    void abort();
    QString encoderName() const;

private:
    MediaWriter();
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// The composition's frame duration as a rational (CompositionBuilder.frameDuration): whole rates exactly,
/// NTSC-style rates (23.976, 29.97, 59.94) as 1001/…
std::pair<int, int> frameDuration(double fps);

} // namespace er
