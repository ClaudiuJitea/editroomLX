#include "Bench.h"
#include "analysis/MediaAnalyzer.h"
#include "audio/Loudness.h"
#include "audio/ReverseAudio.h"
#include "audio/Waveform.h"
#include "engine/Exporter.h"
#include "media/MediaStore.h"
#include "media/Stills.h"
#include "rendering/CpuRenderer.h"
#include "rendering/GpuRenderer.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSysInfo>
#include <QThread>
#include <chrono>
#include <cstdio>
#include <omp.h>

namespace er {

namespace {
void log(const QString& s) { std::fprintf(stdout, "%s\n", s.toUtf8().constData()); std::fflush(stdout); }

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/// Median of 5 runs and the spread (max − min), in milliseconds.
std::pair<double, double> measure(const std::function<void()>& f, int runs = 5) {
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) { const double a = now(); f(); t.push_back((now() - a) * 1000); }
    std::sort(t.begin(), t.end());
    return {t[t.size() / 2], t.back() - t.front()};
}

/// The GPU's own time (timestamp queries), median of 15 frames: one frame's is too noisy on a laptop GPU.
double gpuMedian(GpuRenderer& g, const FrameJob& job) {
    std::vector<double> t;
    for (int i = 0; i < 15; ++i) { g.render(job); t.push_back(g.lastGpuMs()); }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

QString ms(std::pair<double, double> m) { return QString::asprintf("%.2f ms (±%.2f)", m.first, m.second / 2); }

bool ffmpeg(const QStringList& args) {
    QProcess p;
    p.start(QStringLiteral("ffmpeg"), QStringList{"-y", "-loglevel", "error"} + args);
    return p.waitForFinished(600000) && p.exitCode() == 0;
}

QString machine() {
    QFile f(QStringLiteral("/proc/cpuinfo"));
    QString cpu;
    if (f.open(QIODevice::ReadOnly))
        for (const QByteArray& l : f.readAll().split('\n')) if (l.startsWith("model name")) { cpu = QString::fromLatin1(l.mid(l.indexOf(':') + 1).trimmed()); break; }
    return QStringLiteral("%1, %2 threads, %3").arg(cpu).arg(QThread::idealThreadCount()).arg(QSysInfo::prettyProductName());
}
} // namespace

int runBench(const QString& dirIn) {
    // EDITROOM_BENCH=B1,B3 runs only those.
    const QStringList only = qEnvironmentVariable("EDITROOM_BENCH").split(u',', Qt::SkipEmptyParts);
    auto want = [&](const char* b) { return only.isEmpty() || only.contains(QLatin1String(b)); };
    QDir().mkpath(dirIn);
    const QString dir = QDir(dirIn).absolutePath();
    prepareHardwareVideo();
    auto gpu = GpuRenderer::create();
    log(QStringLiteral("BENCH machine %1").arg(machine()));
    log(QStringLiteral("BENCH gpu %1").arg(gpu ? gpu->info().device + QStringLiteral(" (driver ") + gpu->info().driver + QStringLiteral(", Vulkan ") + gpu->info().api + u')' : QStringLiteral("none (CPU only)")));
    log(QStringLiteral("BENCH openmp threads %1").arg(omp_get_max_threads()));
    // Media.
    const QString hevc4k = dir + QStringLiteral("/4k_hevc10.mp4"), h264_4k = dir + QStringLiteral("/4k_h264.mp4"), six = dir + QStringLiteral("/6k_h264.mp4");
    const QString photo = dir + QStringLiteral("/photo_6000x4000.jpg"), song = dir + QStringLiteral("/song_5min.wav");
    if (!QFile::exists(hevc4k)) ffmpeg({"-f", "lavfi", "-i", "testsrc2=size=3840x2160:rate=30:duration=10", "-c:v", "libx265", "-preset", "ultrafast", "-pix_fmt", "yuv420p10le", "-x265-params", "log-level=error", hevc4k});
    if (!QFile::exists(h264_4k)) ffmpeg({"-f", "lavfi", "-i", "testsrc2=size=3840x2160:rate=60:duration=10", "-f", "lavfi", "-i", "sine=frequency=440:duration=10", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", h264_4k});
    if (!QFile::exists(six)) ffmpeg({"-f", "lavfi", "-i", "testsrc2=size=6016x3384:rate=30:duration=3", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", six});
    if (!QFile::exists(photo)) ffmpeg({"-f", "lavfi", "-i", "testsrc2=size=6000x4000", "-frames:v", "1", "-q:v", "3", photo});
    if (!QFile::exists(song)) ffmpeg({"-f", "lavfi", "-i", "aevalsrc='0.5*sin(2*PI*110*t)*exp(-10*mod(t,0.5))+0.1*sin(2*PI*880*t)':s=44100:d=300", "-ac", "2", song});

    MediaStore media;
    SoundStore sound(media);
    auto a4k = media.importFile(hevc4k), b4k = media.importFile(h264_4k), a6k = media.importFile(six), still = media.importFile(photo), tune = media.importFile(song);
    if (!a4k || !b4k || !a6k || !still || !tune) { log(QStringLiteral("BENCH FAIL couldn't make the media")); return 1; }

    // B1: compose 4K — a 4K HEVC 10-bit layer with a grade, a solid dissolving in over it, a caption-sized title.
    if (want("B1"))
    {
        Project p;
        p.width = 3840; p.height = 2160; p.fps = 30;
        p.assets = {*a4k};
        Clip v(p.videoTracks[0].id, ClipKind::video, 0, 8); v.assetID = a4k->id;
        ColorGrade g; g.look = QStringLiteral("Film print"); g.contrast = 10; v.grade = g;
        Clip s(p.videoTracks[0].id, ClipKind::solid, 8, 2); s.transitionIn = Transition{TransitionKind::dissolve, 1.0, {}};
        Clip t(p.videoTracks[1].id, ClipKind::text, 0, 10); t.text.string = QStringLiteral("Benchmark title"); t.text.y = 0.85;
        p.clips = {v, s, t};
        BuildOptions o; o.renderW = 3840; o.renderH = 2160;
        auto plan = *CompositionBuilder::build(p, media, o);
        const FrameJob job = describeFrame(plan, 7.8, media.provider(3840));
        log(QStringLiteral("BENCH B1 compose 3840x2160 (3 layers, grade, dissolve, title) CPU %1").arg(ms(measure([&] { CpuRenderer::render(job); }, 3))));
        if (gpu) {
            gpu->render(job);
            log(QStringLiteral("BENCH B1 compose 3840x2160 GPU %1, GPU time %2 ms").arg(ms(measure([&] { gpu->render(job); }))).arg(gpuMedian(*gpu, job), 0, 'f', 2));
            log(QStringLiteral("BENCH B1 compose+NV12 3840x2160 GPU %1").arg(ms(measure([&] { gpu->renderNV12(job); }))));
        }
        log(QStringLiteral("BENCH B1 describe+decode (frame at 7.8 s, cached chunk) %1").arg(ms(measure([&] { describeFrame(plan, 7.8, media.provider(3840)); }))));
    }
    // B2: compose a 6K source onto a 4K canvas.
    if (want("B2"))
    {
        Project p;
        p.width = 3840; p.height = 2160;
        p.assets = {*a6k};
        Clip v(p.videoTracks[0].id, ClipKind::video, 0, 3); v.assetID = a6k->id;
        p.clips = {v};
        BuildOptions o; o.renderW = 3840; o.renderH = 2160;
        auto plan = *CompositionBuilder::build(p, media, o);
        const FrameJob job = describeFrame(plan, 1.0, media.provider(6016));
        log(QStringLiteral("BENCH B2 compose 6016x3384 -> 3840x2160 CPU %1").arg(ms(measure([&] { CpuRenderer::render(job); }, 3))));
        if (gpu) { gpu->render(job); log(QStringLiteral("BENCH B2 compose 6016x3384 -> 3840x2160 GPU %1, GPU time %2 ms").arg(ms(measure([&] { gpu->render(job); }))).arg(gpuMedian(*gpu, job), 0, 'f', 2)); }
    }
    // B3: a 6000×4000 photo with a transform, a zoom move and a grade.
    if (want("B3"))
    {
        Project p;
        p.width = 3840; p.height = 2160;
        p.assets = {*still};
        Clip c(p.videoTracks[0].id, ClipKind::image, 0, 6); c.assetID = still->id;
        ClipTransform tr; tr.scale = 1.6; tr.x = 0.1; tr.zoom = ZoomMove::starter(ZoomMove::Mode::zoomIn, 6); c.transform = tr;
        ColorGrade g; g.exposure = 0.3; g.saturation = 20; c.grade = g;
        p.clips = {c};
        BuildOptions o; o.renderW = 3840; o.renderH = 2160;
        log(QStringLiteral("BENCH B3 photo 6000x4000 load (decode, orient, 3840 cap) %1").arg(ms(measure([&] { Stills::frame(photo); }, 3))));
        auto plan = *CompositionBuilder::build(p, media, o);
        const FrameJob job = describeFrame(plan, 2.0, {});
        log(QStringLiteral("BENCH B3 photo layer per frame CPU %1").arg(ms(measure([&] { CpuRenderer::render(job); }, 3))));
        if (gpu) {
            log(QStringLiteral("BENCH B3 photo layer first frame GPU (upload) %1").arg(ms(measure([&] { auto g2 = GpuRenderer::create(); g2->render(job); }, 3))));
            gpu->render(job);
            log(QStringLiteral("BENCH B3 photo layer per frame GPU %1, GPU time %2 ms").arg(ms(measure([&] { gpu->render(job); }))).arg(gpuMedian(*gpu, job), 0, 'f', 2));
        }
    }
    // Decode: 4K HEVC 10-bit and 4K60 H.264, hardware vs software (frames per second over 3 s).
    if (want("B4"))
    for (const auto& [label, file] : std::vector<std::pair<QString, QString>>{{"4K HEVC 10-bit", hevc4k}, {"4K60 H.264", h264_4k}}) {
        for (auto accel : {VideoDecoder::Accel::automatic, VideoDecoder::Accel::software}) {
            auto dec = VideoDecoder::open(file, accel);
            if (!dec) continue;
            const double t0 = now();
            int n = 0;
            while (auto f = dec->next()) { ++n; if (f->time > 3) break; }
            log(QStringLiteral("BENCH B4 decode %1 via %2: %3 fps").arg(label, dec->path()).arg(n / (now() - t0), 0, 'f', 1));
        }
    }
    // B6: export 10 s of 4K60 to H.264 High (the hardware encoder, then software).
    if (want("B6"))
    for (const QByteArray& enc : {QByteArray(), QByteArray("libx264")}) {
        if (!enc.isEmpty()) qputenv("EDITROOM_ENCODER", enc); else qunsetenv("EDITROOM_ENCODER");
        Project p;
        p.width = 3840; p.height = 2160; p.fps = 60;
        p.assets = {*b4k};
        Clip v(p.videoTracks[0].id, ClipKind::video, 0, 10); v.assetID = b4k->id;
        Clip a(p.audioTracks[0].id, ClipKind::audio, 0, 10); a.assetID = b4k->id;
        p.clips = {v, a};
        std::atomic<bool> cancel{false};
        auto res = runExport(p, media, sound, gpu.get(), ExportSettings(), dir + QStringLiteral("/bench_export.mp4"), {}, cancel);
        log(res.ok ? QStringLiteral("BENCH B6 export 10 s 3840x2160@60 H.264 High via %1 (%2, decode %3): %4 s, %5x realtime")
                         .arg(res.encoder, res.renderer, res.decode).arg(res.seconds, 0, 'f', 2).arg(10 / res.seconds, 0, 'f', 2)
                   : QStringLiteral("BENCH B6 export FAIL %1").arg(res.error));
    }
    qunsetenv("EDITROOM_ENCODER");
    // B7: reversing the sound of a 5-minute file.
    if (want("B7"))
    {
        QFile::remove(ReverseAudio::cachePath(song));
        const double t0 = now();
        ReverseAudio::render(song);
        log(QStringLiteral("BENCH B7 reverse sound 300 s: %1 s").arg(now() - t0, 0, 'f', 2));
    }
    // B10: loudness of a 60-minute mix, waveform peaks of 5 minutes, beats of 5 minutes.
    if (want("B10"))
    {
        std::vector<float> mix(size_t(48000) * 3600 * 2);
        for (size_t i = 0; i < mix.size() / 2; ++i) mix[i * 2] = mix[i * 2 + 1] = float(0.1 * std::sin(2 * M_PI * 997 * double(i) / 48000));
        log(QStringLiteral("BENCH B10 loudness (BS.1770) 60 min: %1").arg(ms(measure([&] { LoudnessMeter::measure(mix); }, 3))));
        log(QStringLiteral("BENCH B10 waveform 300 s: %1").arg(ms(measure([&] { WaveformReader::read(song); }, 3))));
        auto dec = AudioDecoder::open(song, 22050);
        std::vector<float> st;
        while (dec->read(st, 1 << 16) > 0) {}
        std::vector<float> mono(st.size() / 2);
        for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (st[i * 2] + st[i * 2 + 1]);
        log(QStringLiteral("BENCH B10 beats 300 s: %1").arg(ms(measure([&] { MediaAnalyzer::beats(mono, 22050); }, 3))));
    }
    log(QStringLiteral("BENCH done"));
    return 0;
}

} // namespace er
