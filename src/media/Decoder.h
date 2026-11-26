// FFmpeg probing and video decoding (AVURLAsset / AVAssetReader on the Mac).
//
// Hardware decode first: VA-API, then Vulkan Video, then CUDA (NVDEC); software otherwise. Hardware frames
// come back to memory once (av_hwframe_transfer_data) as NV12 or P010 and are uploaded once to the GPU
// compositor. DMA-BUF import straight into Vulkan is not done yet (docs/PARITY.md); `path()` says which
// way frames came, for the benchmark lines.
#pragma once
#include "rendering/SourceColor.h"
#include "rendering/VideoFrame.h"
#include <QString>
#include <memory>
#include <optional>

namespace er {

struct ProbeResult {
    bool hasVideo = false, hasAudio = false;
    double duration = 0;
    /// After rotation, as the Mac's naturalSize × preferredTransform.
    int width = 0, height = 0;
    double fps = 0;
    int orientation = 1;
    SourceColor color = SourceColor::standard;
    double videoStart = 0, videoEnd = 0, audioEnd = 0;
    int audioRate = 0;
    int audioChannels = 0;
    QString videoCodec, audioCodec;
};

/// Process-wide set-up for hardware video (call before any FFmpeg hardware device is made).
void prepareHardwareVideo();

namespace Probe {
/// Nil when FFmpeg can't open it or it has neither a decodable picture nor sound.
std::optional<ProbeResult> file(const QString& path);
/// EXIF orientation for a display-matrix rotation (degrees counter-clockwise, as FFmpeg gives it).
int orientationForRotation(double degreesCCW);
} // namespace Probe

class VideoDecoder {
public:
    enum class Accel { automatic, software };
    static std::unique_ptr<VideoDecoder> open(const QString& path, Accel accel = Accel::automatic, QString* why = nullptr);
    ~VideoDecoder();

    /// Goes to the keyframe at or before `t` (file seconds); the next frames decoded start there.
    bool seek(double t);
    /// The next frame in decode order, as NV12 / P010 / YUV420P(10) planes. Nil at the end.
    std::optional<VideoFrame> next();
    /// Decodes from wherever it is to the frame showing at `t` (the last one starting at or before it).
    std::optional<VideoFrame> frameAt(double t);

    /// "vaapi", "vulkan", "cuda" or "software".
    QString path() const;
    double fps() const;
    double position() const;
    int width() const;
    int height() const;

private:
    VideoDecoder();
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// Audio decoded as interleaved float stereo at a chosen rate (for the mixer, waveforms, loudness).
class AudioDecoder {
public:
    static std::unique_ptr<AudioDecoder> open(const QString& path, int rate = 48000, QString* why = nullptr);
    ~AudioDecoder();
    bool seek(double t);
    /// Appends up to `frames` stereo frames to `out`; returns how many came (0 at the end).
    int read(std::vector<float>& out, int frames);
    /// The whole file from `t0` to `t1` (seconds).
    std::vector<float> readRange(double t0, double t1);
    int rate() const;
    double duration() const;

private:
    AudioDecoder();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace er
