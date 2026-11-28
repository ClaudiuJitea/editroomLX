// PlaybackEngine (PlaybackEngine.swift): the player. The sound leads: the audio stream's clock says what
// time is being heard, and the render thread composes that frame (GPU, or CPU) for the viewer. A rebuild
// while playing swaps the plans at the playhead with no pause; seeks coalesce (only the newest is shown);
// a fast seek shows whatever frame is ready, an exact one waits for the right one.
//
// With a window surface (the viewer's, given by setPresentSurface) and presenting on, frames go straight into
// its Vulkan swapchain, nothing read back (the Mac's Metal layer); otherwise they're read back as a QImage.
#pragma once
#include "Sound.h"
#include "audio/AudioStream.h"
#include "rendering/GpuRenderer.h"
#include <QImage>
#include <QObject>
#include <array>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <thread>

namespace er {

class PlaybackEngine : public QObject {
    Q_OBJECT
public:
    PlaybackEngine(MediaStore& media, SoundStore& sound, QObject* parent = nullptr);
    ~PlaybackEngine() override;

    void rebuild(const Project& project, double playhead);
    void seek(double t, bool fast = false);
    /// Where made-ahead scrub frames come from (ScrubCache): a video's frame at a time in its file.
    using ScrubFrames = std::function<std::optional<VideoFrame>(const Uuid& asset, double fileTime)>;
    void setScrubFrames(ScrubFrames frames, std::function<bool(const Uuid&)> has);
    /// Shows the frame at `t` composed from made-ahead scrub frames, at once. False when not every video in the
    /// edit has them yet (the caller seeks instead).
    bool showScrubFrame(double t);
    void play();
    void pause();
    bool isPlaying() const { return playing_; }
    double currentTime() const;
    double duration() const { return duration_; }
    bool isEmpty() const { return duration_ <= 0; }
    void setLoopSpan(std::optional<Range> span);
    void setAudible(const std::map<Uuid, bool>& audible);
    /// The time of the frame showing now.
    double shownTime() const { return shownTime_; }
    QString rendererName() const;
    /// The preview's size (CompositionBuilder.previewSize), or another (full screen).
    void setPreviewSize(int w, int h);
    AudioStream& audio() { return audio_; }
    /// The VkInstance a window's surface is made on; none when frames can't be presented.
    void* presentInstance() const { return gpu_ ? gpu_->vkInstance() : nullptr; }
    /// The window surface (VkSurfaceKHR) and its size in device pixels; none to let it go (before it's destroyed).
    bool setPresentSurface(void* surface, int w, int h);
    /// The part of the frame the window shows (0…1): all of it, or what a zoomed viewer has in view.
    void setPresentRegion(double x0, double y0, double x1, double y1);
    /// Frames go to the surface (true) or come back as images (false).
    void setPresenting(bool on);
    bool presenting() const { return presenting_; }
    /// The frame showing now, composed again (after the viewer switched between presenting and painting).
    void redraw();
    GpuRenderer* gpu() { return gpu_.get(); }
    /// Its tab went behind another: playback stops, and the plan, its frames and the sound are let go. The next
    /// rebuild makes them again.
    void unload();
    /// Frames composed and frames the clock had already passed (dropped), since play().
    long long framesShown() const { return framesShown_; }
    long long framesDropped() const { return framesDropped_; }

signals:
    void frameReady(const QImage& frame, double time);
    /// A frame went straight to the window.
    void framePresented(double time);
    void ended();

private:
    void renderLoop();
    MediaStore& media_;
    SoundStore& sound_;
    AudioStream audio_;
    /// One GPU for every tab's player (only the tab in front renders); its window surface is the viewer in front's.
    std::shared_ptr<GpuRenderer> gpu_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::shared_ptr<const RenderPlan> plan_;
    std::optional<double> wanted_;   // a seek to show
    bool wantedFast_ = false;
    bool wantedScrub_ = false;
    ScrubFrames scrubFrames_;
    std::function<bool(const Uuid&)> hasScrub_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> presenting_{false};
    bool hasSurface_ = false;
    std::atomic<double> shownTime_{-1};
    double duration_ = 0;
    double fps_ = 30;
    int previewW_ = 0, previewH_ = 0;
    std::optional<Range> loop_;
    std::array<double, 4> region_{0, 0, 1, 1};
    bool stop_ = false;
    std::atomic<long long> framesShown_{0}, framesDropped_{0};
    std::thread thread_;
    Project project_;
};

} // namespace er
