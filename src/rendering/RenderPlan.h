// RenderPlan: the video side of CompositionBuilder.build plus TimelineInstruction/RenderLayer, ported.
// A plan is built on the main thread from a Project and is immutable; for each output time it gives a
// FrameJob (every number the compositor kernel needs), which the GPU and the CPU renderers both run.
//
// LX has no AVComposition: every video clip, forwards, reversed, HDR or not, is read through our own
// decoder, so every video layer says which file and which source time it shows (the Mac's `.decoded`
// and `.reversed` sources; its `.track` source maps file time the same way through the composition).
#pragma once
#include "ColorMath.h"
#include "SourceColor.h"
#include "VideoFrame.h"
#include "model/Captions.h"
#include <functional>
#include <map>

namespace er {

/// What the media store knows about a file, for building plans (MediaStore.runtimes on the Mac).
struct MediaRuntime {
    int orientation = 1;
    SourceColor color = SourceColor::standard;
    /// Stills only: the decoded picture (EXIF orientation applied).
    std::optional<VideoFrame> still;
    /// The file's length, which a reversed clip's source time is counted back from.
    double length = 0;
    int width = 0, height = 0;
};

class MediaInfo {
public:
    virtual ~MediaInfo() = default;
    virtual const MediaRuntime* runtime(const Uuid& asset) const = 0;
    virtual std::optional<Transcript> transcript(const Uuid&) const { return std::nullopt; }
};

/// A rasterised text or caption: premultiplied RGBA8 at a whole-pixel place (top-left, y down).
struct Pixmap {
    VideoFrame image;
    int x = 0, y = 0;
};

struct PlanLayer {
    enum class Kind { video, image, solid, text } kind = Kind::solid;
    /// When the layer is drawn: the clip, plus any extra a transition shows before or after it.
    double start = 0, end = 0;
    /// The clip itself, which its own fades are measured from.
    double fadeStart = 0, fadeEnd = 0;
    std::optional<TransitionEdge> reveal, conceal;
    double fadeIn = 0, fadeOut = 0;
    FadeCurve inCurve = FadeCurve::linear, outCurve = FadeCurve::linear;
    std::optional<ClipTransform> transform;
    std::shared_ptr<const std::vector<float>> cube;
    // Video
    Uuid asset;
    std::optional<SourceClock> clock;
    bool reversed = false;
    double length = 0;
    int orientation = 1;
    SourceColor color = SourceColor::standard;
    // Image
    std::optional<VideoFrame> still;
    // Solid (premultiplied in the kernel)
    RGBA rgba;
    // Text
    std::shared_ptr<const Pixmap> text;

    double opacity(double t) const;
    /// For a wipe in progress, how much of the picture (from the left) is showing.
    std::optional<double> wipe(double t) const;
    /// The file time a video layer shows at timeline time t (with the Mac's 1e-4 nudge).
    double sourceTime(double t) const;
};

/// One caption as it's drawn (CaptionLayer).
struct CaptionPlanLayer {
    double start = 0, end = 0;
    QStringList words;
    std::optional<int> lit;
    CaptionStyle style;
};

struct RenderPlan {
    /// The sequence is laid out at this size; the output is the window at `offset` (y down) within it.
    int canvasW = 0, canvasH = 0;
    int outW = 0, outH = 0;
    double offsetX = 0, offsetY = 0;
    double duration = 0;
    double fps = 30;
    /// Bottom-most first.
    std::vector<PlanLayer> layers;
    std::vector<CaptionPlanLayer> captions;
    uint64_t signature = 0;
    /// Video drawn bigger than its own pixels is enlarged with the upscaler (on export), not just stretched.
    bool upscale = false;

    bool isEmpty() const { return duration <= 0; }
};

struct BuildOptions {
    int renderW = 0, renderH = 0;
    /// Scales the sequence to cover the render size and crops around this point (0…1 across and down).
    std::optional<std::pair<double, double>> reframe;
    /// The player keeps hidden tracks out of the picture but in the plan's signature.
    bool keepAllTracks = false;
    /// Export: video drawn bigger than its pixels (a 1080p sequence at 4K) is upscaled, not stretched.
    bool upscale = false;
};

namespace CompositionBuilder {
std::optional<RenderPlan> build(const Project& project, const MediaInfo& media, const BuildOptions& options);
/// The preview's size: the sequence, no bigger than 1920 on its long side, even numbers.
std::pair<int, int> previewSize(const Project& project);
} // namespace CompositionBuilder

/// A picture for a video layer at a time; nullopt when it isn't decoded (yet).
using FrameProvider = std::function<std::optional<VideoFrame>(int layerIndex, const PlanLayer& layer, double sourceTime)>;

/// Everything one frame's composite needs, as the kernel's descriptors (ColorMath.h).
struct FrameJob {
    cm::ErFrame frame{};
    std::vector<cm::ErLayer> layers;
    std::vector<cm::ErSource> sources;
    /// The pictures, by source index (their bytes are uploaded once per frame, or reused by key).
    std::vector<VideoFrame> pictures;
    /// The cubes, concatenated; `cubeKeys` names each (pointer identity) with its float offset.
    std::vector<float> cubes;
    std::vector<std::pair<const void*, int>> cubeKeys;
    double time = 0;
};

/// The frame at timeline time `t` (TimelineCompositor.compose): every layer showing then.
FrameJob describeFrame(const RenderPlan& plan, double t, const FrameProvider& frames);

namespace TextRenderer {
/// The text drawn at its place on a canvas of this size (cached, 64 entries, as on the Mac).
std::shared_ptr<const Pixmap> image(const TextStyle& style, int canvasW, int canvasH);
}

namespace CaptionRenderer {
/// Each caption, split by word when the word being said lights up.
std::vector<CaptionPlanLayer> layers(const std::vector<CaptionBlock>& blocks, const CaptionStyle& style);
/// The caption placed on a frame of this size (cached, 400 entries).
std::shared_ptr<const Pixmap> image(const QStringList& words, std::optional<int> lit, const CaptionStyle& style, int outW, int outH);
}

} // namespace er
