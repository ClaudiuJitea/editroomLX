#include "Exporter.h"
#include "MediaWriter.h"
#include "audio/Loudness.h"
#include "audio/Mixer.h"
#include "rendering/CpuRenderer.h"
#include <QFileInfo>
#include <QLoggingCategory>
#include <chrono>
#include <cmath>
#include <future>

namespace er {

ExportResult runExport(const Project& whole, MediaStore& media, SoundStore& sound, GpuRenderer* gpu,
                       const ExportSettings& settings, const QString& path,
                       const std::function<void(double)>& progress, const std::atomic<bool>& cancel) {
    ExportResult res;
    const auto t0 = std::chrono::steady_clock::now();
    Project project = settings.output(whole);
    const bool wav = settings.format == ExportSettings::Format::wav;
    const bool wantsAudio = wav || settings.includeAudio;
    const auto [w, h] = settings.renderSize(project);
    if (!(project.duration() > 0)) { res.error = QStringLiteral("The timeline is empty."); return res; }

    // Sound (reversed clips' sound made first, waveforms read for ducking).
    AudioPlan audio;
    if (wantsAudio) {
        audio = AudioPlanBuilder::build(project, sound.sources(true));
        if (settings.loudness && !audio.isEmpty()) {
            // Measure the mix first, then export with every track turned by the same amount, kept 1 dB
            // under full scale so the result can't clip.
            Mixer meter;
            meter.setPlan(audio);
            const auto mix = meter.renderRange(0, project.duration());
            if (cancel) { res.cancelled = true; return res; }
            if (auto r = LoudnessMeter::measure(mix)) {
                const double gain = LoudnessMeter::targetGain(*r, *settings.loudness);
                for (auto& t : project.audioTracks) t.gainDB += gain;
                audio = AudioPlanBuilder::build(project, sound.sources(true));
            }
        }
    }
    if (wav && audio.isEmpty()) { res.error = QStringLiteral("There's no audio on the timeline."); return res; }

    WriterSettings ws;
    std::optional<RenderPlan> plan;
    const auto fd = frameDuration(project.fps);
    if (!wav) {
        BuildOptions o;
        o.renderW = w;
        o.renderH = h;
        if (settings.crops(project)) o.reframe = std::make_pair(settings.reframeX, settings.reframeY);
        plan = CompositionBuilder::build(project, media, o);
        if (!plan) { res.error = QStringLiteral("The timeline is empty."); return res; }
        WriterSettings::Video v;
        v.width = w;
        v.height = h;
        v.fpsNum = fd.first;
        v.fpsDen = fd.second;
        v.hevc = settings.codec == ExportSettings::Codec::hevc;
        v.bitrate = settings.videoBitrate(w, h, project.fps);
        v.gop = int(std::round(project.fps * 2));
        ws.video = v;
    }
    if (!audio.isEmpty()) {
        WriterSettings::Audio a;
        a.wav = wav;
        a.bitrate = settings.quality == ExportSettings::Quality::high ? 256000 : 160000;
        ws.audio = a;
    }
    QString why;
    auto writer = MediaWriter::open(path, ws, &why);
    if (!writer) { res.error = why; return res; }
    res.encoder = writer->encoderName();
    res.renderer = gpu ? gpu->info().device : QStringLiteral("CPU (OpenMP)");

    Mixer mixer;
    mixer.setPlan(audio);
    const double duration = project.duration();
    const double frameSeconds = double(fd.first) / fd.second;
    const long long frames = wav ? 0 : (long long)std::ceil(duration / frameSeconds - 1e-9);
    const int limit = std::max(w, h);
    auto provider = media.provider(std::max(limit, std::max(plan ? plan->canvasW : 0, plan ? plan->canvasH : 0)), true);
    long long audioFrames = 0;
    const long long totalAudio = (long long)std::llround(duration * 48000);
    auto writeAudioUntil = [&](double t) {
        const long long until = std::min(totalAudio, (long long)std::llround(t * 48000));
        while (audioFrames < until && !audio.isEmpty()) {
            const int n = int(std::min<long long>(4800, until - audioFrames));
            std::vector<float> buf;
            mixer.render(double(audioFrames) / 48000, n, buf);
            if (!writer->writeAudio(buf.data(), n)) return false;
            audioFrames += n;
        }
        return true;
    };
    // The next frame is described (its pictures decoded) while this one renders and encodes.
    auto describe = [&](long long k) { return describeFrame(*plan, double(k) * frameSeconds, provider); };
    std::future<FrameJob> next;
    if (frames > 0) next = std::async(std::launch::async, describe, 0);
    double lastReported = -1;
    for (long long k = 0; k < frames; ++k) {
        if (cancel) { writer->abort(); res.cancelled = true; return res; }
        FrameJob job = next.get();
        if (k + 1 < frames) next = std::async(std::launch::async, describe, k + 1);
        std::vector<uint8_t> nv12 = gpu ? gpu->renderNV12(job) : std::vector<uint8_t>();
        if (nv12.empty()) nv12 = CpuRenderer::renderNV12(job);
        if (!writer->writeVideo(nv12.data(), k)) { writer->abort(); res.error = QStringLiteral("The encoder stopped."); return res; }
        if (!writeAudioUntil(double(k + 1) * frameSeconds)) { writer->abort(); res.error = QStringLiteral("The encoder stopped."); return res; }
        const double p = double(k + 1) / double(frames);
        if (p - lastReported >= 0.005) { lastReported = p; if (progress) progress(std::min(1.0, p)); }
        if (k == 0 && !job.pictures.empty()) {
            for (const auto& l : plan->layers) if (l.kind == PlanLayer::Kind::video) { res.decode = media.decodePath(l.asset); break; }
        }
    }
    // The sound (all of it, for WAV; the rest, for video).
    while (audioFrames < totalAudio && !audio.isEmpty()) {
        if (cancel) { writer->abort(); res.cancelled = true; return res; }
        if (!writeAudioUntil(double(audioFrames + 48000) / 48000)) { writer->abort(); res.error = QStringLiteral("The encoder stopped."); return res; }
        if (wav && progress) progress(double(audioFrames) / std::max<long long>(1, totalAudio));
    }
    if (!writer->finish()) { res.error = QStringLiteral("Couldn't finish the file."); return res; }
    if (progress) progress(1);
    res.ok = true;
    res.frames = frames;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

// MARK: Queue

QString ExportQueue::Item::name() const { return QFileInfo(path).fileName(); }

ExportQueue::ExportQueue(MediaStore& media, SoundStore& sound, QObject* parent) : QObject(parent), media_(media), sound_(sound) {
    clearTimer_.setSingleShot(true);
    clearTimer_.setInterval(12000);
    connect(&clearTimer_, &QTimer::timeout, this, [this] {
        if (!isBusy() && !holdOpen_) clearFinished();
    });
}

ExportQueue::~ExportQueue() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

const ExportQueue::Item* ExportQueue::current() const {
    for (const auto& i : items_) if (i.status == Status::running) return &i;
    return nullptr;
}

int ExportQueue::waiting() const {
    int n = 0;
    for (const auto& i : items_) if (i.status == Status::waiting) ++n;
    return n;
}

bool ExportQueue::isBusy() const {
    for (const auto& i : items_) if (i.status == Status::running || i.status == Status::waiting) return true;
    return false;
}

std::optional<double> ExportQueue::timeLeft() const {
    if (!current() || !(progress_ > 0.03)) return std::nullopt;
    const double elapsed = startedAt_.msecsTo(QDateTime::currentDateTime()) / 1000.0;
    return elapsed / progress_ * (1 - progress_);
}

void ExportQueue::add(const Project& project, const ExportSettings& settings, const QString& path) {
    clearTimer_.stop();
    Item item;
    item.path = path;
    item.project = project;
    item.settings = settings;
    items_.push_back(item);
    emit changed();
    startNext();
}

void ExportQueue::cancel(const Uuid& id) {
    for (auto& i : items_) {
        if (i.id != id) continue;
        if (i.status == Status::running) cancel_ = true;
        else if (i.status == Status::waiting) { i.status = Status::cancelled; emit changed(); }
    }
}

void ExportQueue::clearFinished() {
    std::erase_if(items_, [](const Item& i) { return i.status != Status::running && i.status != Status::waiting; });
    emit changed();
}

void ExportQueue::setHoldOpen(bool hold) {
    holdOpen_ = hold;
    if (!hold && !isBusy()) scheduleClear();
}

void ExportQueue::scheduleClear() { clearTimer_.start(); }

void ExportQueue::startNext() {
    if (current()) return;
    Item* next = nullptr;
    for (auto& i : items_) if (i.status == Status::waiting) { next = &i; break; }
    if (!next) {
        if (!isBusy() && !items_.empty()) scheduleClear();
        return;
    }
    if (worker_.joinable()) worker_.join();
    if (!gpuTried_) { gpuTried_ = true; gpu_ = GpuRenderer::create(); }
    next->status = Status::running;
    progress_ = 0;
    startedAt_ = QDateTime::currentDateTime();
    cancel_ = false;
    const Item job = *next;
    emit changed();
    worker_ = std::thread([this, job] {
        auto res = runExport(job.project, media_, sound_, gpu_.get(), job.settings, job.path,
                             [this](double p) { QMetaObject::invokeMethod(this, [this, p] { progress_ = std::max(progress_, p); emit changed(); }, Qt::QueuedConnection); },
                             cancel_);
        QMetaObject::invokeMethod(this, [this, id = job.id, res, path = job.path] {
            for (auto& i : items_) {
                if (i.id != id) continue;
                i.status = res.ok ? Status::done : res.cancelled ? Status::cancelled : Status::failed;
                i.message = res.error;
            }
            emit finished(path, res.ok, res.error);
            emit changed();
            startNext();
        }, Qt::QueuedConnection);
    });
}

} // namespace er
