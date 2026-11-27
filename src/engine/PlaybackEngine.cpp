#include "PlaybackEngine.h"
#include "rendering/CpuRenderer.h"
#include <chrono>
#include <cmath>
#include <cstring>

namespace er {

PlaybackEngine::PlaybackEngine(MediaStore& media, SoundStore& sound, QObject* parent)
    : QObject(parent), media_(media), sound_(sound) {
    gpu_ = GpuRenderer::create();
    thread_ = std::thread([this] { renderLoop(); });
}

PlaybackEngine::~PlaybackEngine() {
    {
        std::lock_guard g(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

bool PlaybackEngine::setPresentSurface(void* surface, int w, int h) {
    if (!gpu_) return false;
    const bool ok = gpu_->setSurface(surface, w, h);
    hasSurface_ = surface && ok;
    if (!hasSurface_) presenting_ = false;
    return ok;
}

void PlaybackEngine::setPresenting(bool on) {
    const bool want = on && hasSurface_;
    if (want == presenting_) return;
    presenting_ = want;
    redraw();
}

void PlaybackEngine::redraw() {
    if (playing_) return;   // the next frame comes anyway
    const double t = shownTime_;
    if (t >= 0) seek(t, false);
}

QString PlaybackEngine::rendererName() const { return gpu_ ? gpu_->info().device : QStringLiteral("CPU"); }

void PlaybackEngine::setPreviewSize(int w, int h) {
    std::lock_guard g(lock_);
    previewW_ = w;
    previewH_ = h;
}

void PlaybackEngine::rebuild(const Project& project, double playhead) {
    auto [w, h] = CompositionBuilder::previewSize(project);
    {
        std::lock_guard g(lock_);
        if (previewW_ > 0 && previewH_ > 0) { w = previewW_; h = previewH_; }
    }
    BuildOptions o;
    o.renderW = w;
    o.renderH = h;
    o.keepAllTracks = true;
    auto plan = CompositionBuilder::build(project, media_, o);
    const AudioPlan audio = AudioPlanBuilder::build(project, sound_.sources(false), true);
    audio_.setPlan(audio);
    std::map<Uuid, bool> audible;
    for (const auto& t : project.audioTracks) audible[t.id] = project.isAudible(t);
    audio_.setAudible(audible);
    {
        std::lock_guard g(lock_);
        plan_ = plan ? std::make_shared<const RenderPlan>(std::move(*plan)) : nullptr;
        duration_ = project.duration();
        fps_ = project.fps;
        project_ = project;
        if (!playing_) { wanted_ = playhead; wantedFast_ = false; }
    }
    wake_.notify_all();
}

void PlaybackEngine::seek(double t, bool fast) {
    if (playing_) audio_.seek(t);
    {
        std::lock_guard g(lock_);
        wanted_ = t;
        wantedFast_ = fast;
        wantedScrub_ = false;
    }
    wake_.notify_all();
}

void PlaybackEngine::setScrubFrames(ScrubFrames frames, std::function<bool(const Uuid&)> has) {
    std::lock_guard g(lock_);
    scrubFrames_ = std::move(frames);
    hasScrub_ = std::move(has);
}

bool PlaybackEngine::showScrubFrame(double t) {
    if (playing_) return false;
    {
        std::lock_guard g(lock_);
        if (!plan_ || !scrubFrames_ || !hasScrub_) return false;
        const double at = std::clamp(t, 0.0, std::max(0.0, plan_->duration)) + 1e-4;
        for (const auto& L : plan_->layers)
            if (L.kind == PlanLayer::Kind::video && at >= L.start && at < L.end && !hasScrub_(L.asset)) return false;
        wanted_ = t;
        wantedFast_ = true;
        wantedScrub_ = true;
    }
    wake_.notify_all();
    return true;
}

void PlaybackEngine::play() {
    double from;
    {
        std::lock_guard g(lock_);
        from = wanted_.value_or(shownTime_ >= 0 ? double(shownTime_) : 0.0);
        wanted_.reset();
        framesShown_ = 0;
        framesDropped_ = 0;
    }
    audio_.play(from);
    playing_ = true;
    wake_.notify_all();
}

void PlaybackEngine::pause() {
    const double t = audio_.currentTime();
    playing_ = false;
    audio_.pause();
    std::lock_guard g(lock_);
    shownTime_ = t;
}

double PlaybackEngine::currentTime() const { return playing_ ? audio_.currentTime() : double(shownTime_ < 0 ? 0 : shownTime_.load()); }

void PlaybackEngine::setLoopSpan(std::optional<Range> span) {
    audio_.setLoop(span ? std::optional<std::pair<double, double>>(std::make_pair(span->start, span->end)) : std::nullopt);
    std::lock_guard g(lock_);
    loop_ = span;
}

void PlaybackEngine::setAudible(const std::map<Uuid, bool>& audible) { audio_.setAudible(audible); }

void PlaybackEngine::renderLoop() {
    std::unique_lock g(lock_);
    double lastFrameTime = -1;
    while (!stop_) {
        if (!playing_ && !wanted_) {
            wake_.wait_for(g, std::chrono::milliseconds(50));
            continue;
        }
        auto plan = plan_;
        double t;
        bool exact = true, scrub = false;
        const ScrubFrames scrubFrames = scrubFrames_;
        if (playing_) {
            // The frame for what's being heard.
            // (currentTime already allows for the output's delay, Bluetooth headphones included.)
            t = audio_.currentTime();
            const double frame = std::floor(t * fps_ + 1e-6) / fps_;
            if (frame == lastFrameTime) {
                g.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                g.lock();
                continue;
            }
            t = frame;
            exact = false;
            if (!loop_ && t >= duration_ && audio_.ended()) {
                playing_ = false;
                QMetaObject::invokeMethod(this, [this] { emit ended(); }, Qt::QueuedConnection);
                continue;
            }
        } else {
            t = *wanted_;
            exact = !wantedFast_;
            scrub = wantedScrub_;
            wanted_.reset();
        }
        g.unlock();
        QImage image;
        bool presented = false;
        if (plan) {
            const int limit = std::max(plan->canvasW, plan->canvasH);
            // Playing and fast seeks take what's decoded; exact seeks wait for the right frame.
            FrameProvider provider = media_.provider(limit, exact || playing_);
            // Scrubbing: videos from their made-ahead frames, so the picture keeps up with the mouse.
            if (scrub && scrubFrames) {
                provider = [scrubFrames, fallback = provider](int i, const PlanLayer& layer, double sourceTime) -> std::optional<VideoFrame> {
                    if (layer.kind == PlanLayer::Kind::video) if (auto f = scrubFrames(layer.asset, sourceTime)) return f;
                    return fallback(i, layer, sourceTime);
                };
            }
            const FrameJob job = describeFrame(*plan, t, provider);
            // Straight into the viewer's window when it's presenting; else read back as an image.
            if (presenting_ && gpu_ && gpu_->present(job)) {
                presented = true;
            } else {
                std::vector<uint8_t> px = gpu_ ? gpu_->render(job) : std::vector<uint8_t>();
                if (px.empty()) px = CpuRenderer::render(job);
                image = QImage(plan->outW, plan->outH, QImage::Format_RGBX8888);
                for (int y = 0; y < plan->outH; ++y) std::memcpy(image.scanLine(y), px.data() + size_t(y) * plan->outW * 4, size_t(plan->outW) * 4);
            }
        }
        g.lock();
        if (playing_) {
            const double now = audio_.currentTime();
            if (now - t > 1.5 / fps_) ++framesDropped_;
            ++framesShown_;
        }
        lastFrameTime = t;
        shownTime_ = t;
        if (!image.isNull()) QMetaObject::invokeMethod(this, [this, image, t] { emit frameReady(image, t); }, Qt::QueuedConnection);
        else if (presented) QMetaObject::invokeMethod(this, [this, t] { emit framePresented(t); }, Qt::QueuedConnection);
    }
}

} // namespace er
