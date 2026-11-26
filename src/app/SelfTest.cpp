#include "SelfTest.h"
#include "editor/EditorModel.h"
#include "engine/Exporter.h"
#include "engine/ScrubCache.h"
#include "media/Decoder.h"
#include "media/MediaStore.h"
#include "model/Codec.h"
#include "model/Json.h"
#include "rendering/CpuRenderer.h"
#include "rendering/GpuRenderer.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

namespace er {

namespace {
void log(const QString& s) { std::fprintf(stderr, "%s\n", s.toUtf8().constData()); std::fflush(stderr); }

void save(const Project& p, const QString& path) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(json::write(codec::encode(SavedDocument{1, p, {}}), true));
}

bool exportTo(const Project& p, MediaStore& media, SoundStore& sound, GpuRenderer* gpu, const ExportSettings& s, const QString& path) {
    std::atomic<bool> cancel{false};
    const auto r = runExport(p, media, sound, gpu, s, path, [](double) {}, cancel);
    if (r.ok) log(QStringLiteral("EXPORT OK %1 (%2 frames, %3, %4)").arg(QFileInfo(path).fileName()).arg(r.frames).arg(r.encoder, r.renderer));
    else log(QStringLiteral("EXPORT FAIL %1: %2").arg(QFileInfo(path).fileName(), r.error));
    return r.ok;
}

using Patches = std::vector<std::array<int, 3>>;
const Patches expected = {{118, 118, 118}, {235, 235, 235}, {200, 40, 40}, {40, 170, 60}, {40, 60, 200}, {224, 172, 105}};

/// Six patch colours across the picture, 3/10 of the way down (FormatCheck's sampling).
Patches sample(const uint8_t* rgba, int w, int h, int stride) {
    Patches out;
    for (int i = 0; i < 6; ++i) {
        const int x = (2 * i + 1) * w / 12, y = h * 3 / 10;
        const uint8_t* p = rgba + size_t(y) * stride + size_t(x) * 4;
        out.push_back({p[0], p[1], p[2]});
    }
    return out;
}

std::optional<Patches> frame(const Project& p, MediaStore& media, GpuRenderer* gpu, double at = 0.5) {
    BuildOptions o;
    o.renderW = p.width;
    o.renderH = p.height;
    auto plan = CompositionBuilder::build(p, media, o);
    if (!plan) return std::nullopt;
    const FrameJob job = describeFrame(*plan, at, media.provider(std::max(p.width, p.height)));
    const auto px = gpu ? gpu->render(job) : CpuRenderer::render(job);
    if (px.size() < size_t(p.width) * p.height * 4) return std::nullopt;
    return sample(px.data(), p.width, p.height, p.width * 4);
}

std::optional<Patches> exportedFrame(const QString& path, double at) {
    auto dec = VideoDecoder::open(path, VideoDecoder::Accel::software);
    if (!dec) return std::nullopt;
    auto f = dec->frameAt(at);
    if (!f) return std::nullopt;
    const QImage img = ScrubCache::image(*f, 100000).convertToFormat(QImage::Format_RGBA8888);
    return sample(img.constBits(), img.width(), img.height(), int(img.bytesPerLine()));
}

QString patchText(const Patches& px) {
    QStringList s;
    for (const auto& p : px) s << QStringLiteral("%1,%2,%3").arg(p[0]).arg(p[1]).arg(p[2]);
    return s.join(QStringLiteral("  "));
}

int worst(const Patches& px) {
    int w = 0;
    for (size_t i = 0; i < px.size() && i < expected.size(); ++i)
        for (int c = 0; c < 3; ++c) w = std::max(w, std::abs(px[i][size_t(c)] - expected[i][size_t(c)]));
    return w;
}

bool isPatches(const QString& name) {
    for (const char* p : {"sdr_", "hlg_", "pq_", "applelog_"}) if (name.startsWith(QLatin1String(p))) return true;
    return false;
}

QStringList filesIn(const QString& dir) {
    QStringList out;
    for (const auto& fi : QDir(dir).entryInfoList(QDir::Files, QDir::Name)) out << fi.absoluteFilePath();
    return out;
}
} // namespace

int runSelfTest(const QString& mediaDir, const QString& outDir) {
    QDir().mkpath(outDir);
    auto gpu = GpuRenderer::create();
    log(QStringLiteral("renderer: %1").arg(gpu ? QStringLiteral("GPU") : QStringLiteral("CPU")));
    MediaStore media;
    SoundStore sound(media);
    Project project;
    for (const QString& f : filesIn(mediaDir)) {
        if (auto a = media.importFile(f)) {
            project.assets.push_back(*a);
            log(QStringLiteral("IMPORT OK %1 kind=%2 dur=%3 %4x%5 fps=%6 audio=%7")
                    .arg(QFileInfo(f).fileName())
                    .arg(a->kind == MediaKind::video ? QStringLiteral("video") : a->kind == MediaKind::audio ? QStringLiteral("audio") : QStringLiteral("image"))
                    .arg(a->duration).arg(a->width).arg(a->height).arg(a->fps).arg(a->hasAudio ? QStringLiteral("true") : QStringLiteral("false")));
        } else {
            log(QStringLiteral("IMPORT FAIL %1").arg(QFileInfo(f).fileName()));
        }
    }
    const MediaAsset* video = nullptr;
    for (const auto& a : project.assets) if (a.kind == MediaKind::video && a.hasAudio) { video = &a; break; }
    if (!video) { log(QStringLiteral("SELFTEST FAIL no video asset")); return 1; }
    project.width = 1280;
    project.height = 720;
    project.fps = 30;
    const Uuid v1 = project.videoTracks[0].id, v2 = project.videoTracks[1].id, a1 = project.audioTracks[0].id, a2 = project.audioTracks[1].id;
    // 0-2s normal speed, 2-3s at 400% (source 2s..6s), then a solid fading in.
    const Uuid l1 = newUuid(), l2 = newUuid();
    Clip c1(v1, ClipKind::video, 0, 2); c1.assetID = video->id; c1.linkID = l1; c1.fadeIn = 0.5;
    Clip c1a(a1, ClipKind::audio, 0, 2); c1a.assetID = video->id; c1a.linkID = l1; c1a.fadeIn = 0.5;
    Clip c2(v1, ClipKind::video, 2, 1, 2, 4); c2.assetID = video->id; c2.linkID = l2;
    Clip c2a(a1, ClipKind::audio, 2, 1, 2, 4); c2a.assetID = video->id; c2a.linkID = l2;
    Clip solid(v1, ClipKind::solid, 3.5, 1);
    solid.color = RGBA{0.9, 0.2, 0.2, 1, {}};
    solid.fadeIn = 0.5;
    ClipTransform tr;
    tr.scale = 0.5; tr.x = 0.25; tr.y = -0.25;
    solid.transform = tr;
    Clip text(v2, ClipKind::text, 0.5, 2);
    text.text.string = QStringLiteral("Editroom");
    text.fadeIn = 0.5;
    text.fadeOut = 0.5;
    text.fadeOutCurve = FadeCurve::smooth;
    // The sped-up section graded: mono look plus a strong warm push in the highlights; a dissolve into it.
    ColorGrade grade;
    grade.look = QStringLiteral("Mono");
    grade.highlights = WheelPoint{1, 0};
    c2.grade = grade;
    c2.transitionIn = Transition{TransitionKind::dissolve, 0.5, {}};
    c1.fadeInCurve = FadeCurve::easeIn;
    c1a.fadeInCurve = FadeCurve::easeIn;
    project.clips = {c1, c1a, c2, c2a, solid, text};
    for (const auto& a : project.assets) if (a.kind == MediaKind::video && a.id != video->id) {
        Clip pip(v2, ClipKind::video, 2.5, std::min(1.0, a.duration));
        pip.assetID = a.id;
        project.clips.push_back(pip);
        break;
    }
    for (const auto& a : project.assets) if (a.kind == MediaKind::audio) {
        Clip t(a2, ClipKind::audio, 1, std::min(3.0, a.duration));
        t.assetID = a.id;
        t.gainDB = -6;
        t.fadeOut = 1;
        project.clips.push_back(t);
        break;
    }
    project.sanitize();
    log(QStringLiteral("project duration %1").arg(project.duration()));
    save(project, outDir + QStringLiteral("/selftest.room"));

    bool ok = true;
    ExportSettings s;
    ok = exportTo(project, media, sound, gpu.get(), s, outDir + QStringLiteral("/edit_h264.mp4")) && ok;
    s.codec = ExportSettings::Codec::hevc;
    s.quality = ExportSettings::Quality::web;
    s.height = 2160;
    ok = exportTo(project, media, sound, gpu.get(), s, outDir + QStringLiteral("/edit_hevc_4k_web.mp4")) && ok;
    s = ExportSettings();
    s.includeAudio = false;
    s.height = 360;
    ok = exportTo(project, media, sound, gpu.get(), s, outDir + QStringLiteral("/edit_noaudio.mp4")) && ok;
    s = ExportSettings();
    s.format = ExportSettings::Format::wav;
    ok = exportTo(project, media, sound, gpu.get(), s, outDir + QStringLiteral("/edit.wav")) && ok;
    log(ok ? QStringLiteral("SELFTEST OK") : QStringLiteral("SELFTEST FAIL"));
    return ok ? 0 : 1;
}

int runFormatCheck(const QString& outDir, const QStringList& files) {
    QDir().mkpath(outDir);
    auto gpu = GpuRenderer::create();
    for (const QString& file : files) {
        const QString name = QFileInfo(file).fileName();
        MediaStore media;
        SoundStore sound(media);
        QElapsedTimer t0;
        t0.start();
        auto a = media.importFile(file);
        if (!a) { log(QStringLiteral("FORMAT %1: IMPORT FAILED").arg(name)); continue; }
        const auto probe = media.probe(a->id);
        const QString audio = probe && probe->hasAudio ? QStringLiteral("%1 %2ch %3Hz").arg(probe->audioCodec).arg(probe->audioChannels).arg(probe->audioRate)
                                                       : QStringLiteral("none");
        const MediaRuntime* rt = media.runtime(a->id);
        log(QStringLiteral("FORMAT %1: imported in %2s as %3 %4x%5 %6 fps %7s, audio %8, colour %9")
                .arg(name, QString::number(t0.elapsed() / 1000.0, 'f', 2), a->kind == MediaKind::video ? QStringLiteral("video") : QStringLiteral("audio"))
                .arg(a->width).arg(a->height).arg(QString::number(a->fps, 'f', 3), QString::number(a->duration, 'f', 2), audio)
                .arg(rt ? int(rt->color) : 0));
        if (a->kind != MediaKind::video) continue;
        Project p;
        p.width = a->width;
        p.height = a->height;
        p.fps = EditorModel::standardFPS(a->fps);
        p.assets = {*a};
        Clip clip(p.videoTracks[0].id, ClipKind::video, 0, std::min(a->duration, 1.5));
        clip.assetID = a->id;
        p.clips = {clip};
        const bool patches = isPatches(name);
        auto report = [&](const QString& label, const std::optional<Patches>& px) {
            if (!px) { log(QStringLiteral("FORMAT %1: %2 NO FRAME").arg(name, label)); return; }
            if (!patches) { log(QStringLiteral("FORMAT %1: %2 frame ok").arg(name, label)); return; }
            log(QStringLiteral("FORMAT %1: %2 worst off by %3  [%4]").arg(name, label).arg(worst(*px)).arg(patchText(*px)));
        };
        report(QStringLiteral("playback"), frame(p, media, gpu.get()));
        Project rev = p;
        rev.clips[0].reversed = true;
        report(QStringLiteral("reversed"), frame(rev, media, gpu.get()));
        for (double at : {0.2, std::min(1.0, a->duration * 0.6)}) report(QStringLiteral("preview at %1s").arg(at, 0, 'f', 1), frame(p, media, nullptr, at));
        const QString out = outDir + u'/' + QFileInfo(file).completeBaseName() + QStringLiteral(".mp4");
        if (!exportTo(p, media, sound, gpu.get(), ExportSettings(), out)) { log(QStringLiteral("FORMAT %1: EXPORT FAILED").arg(name)); continue; }
        const auto pr = Probe::file(out);
        report(QStringLiteral("export (%1s)").arg(pr ? pr->duration : 0, 0, 'f', 2), exportedFrame(out, 0.5));
    }
    // Every video clip in a row: a timeline mixing formats, as people really edit.
    MediaStore mixed;
    Project p;
    for (const QString& f : files) if (auto a = mixed.importFile(f)) p.assets.push_back(*a);
    double t = 0;
    for (const auto& a : p.assets) {
        if (a.kind != MediaKind::video) continue;
        Clip c(p.videoTracks[0].id, ClipKind::video, t, std::min(a.duration, 2.0));
        c.assetID = a.id;
        p.clips.push_back(c);
        t += c.duration;
    }
    p.width = 1920;
    p.height = 1080;
    double start = 0;
    for (const auto& c : p.clips) {
        const QString name = p.asset(c.assetID) ? p.asset(c.assetID)->name : QStringLiteral("?");
        const auto px = frame(p, mixed, gpu.get(), start + 1);
        if (px && isPatches(name)) log(QStringLiteral("FORMAT mixed timeline, %1: worst off by %2  [%3]").arg(name).arg(worst(*px)).arg(patchText(*px)));
        else log(QStringLiteral("FORMAT mixed timeline, %1: %2").arg(name, px ? QStringLiteral("frame ok") : QStringLiteral("NO FRAME")));
        start += c.duration;
    }
    save(p, outDir + QStringLiteral("/formats.room"));
    return 0;
}

} // namespace er
