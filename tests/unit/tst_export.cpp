// Export: ExportSettings maths (SelfTest), MP4 and WAV files with the Mac's settings (BT.709 tags, AAC,
// 24-bit WAV, keyframes every 2 s), the in/out range (RANGE), the loudness target, and the encoded
// picture against the pre-encode frame (PSNR ≥ 40 dB).
#include "../RenderHelpers.h"
#include "audio/Loudness.h"
#include "engine/Exporter.h"
#include "engine/MediaWriter.h"
#include "media/Decoder.h"
#include "rendering/CpuRenderer.h"
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace er;

class TestExport : public QObject {
    Q_OBJECT
    static QString clip(const char* n) { return QStringLiteral(ER_TEST_MEDIA "/") + QLatin1String(n); }
    MediaStore media;
    std::unique_ptr<SoundStore> sound;
    std::unique_ptr<GpuRenderer> gpu;
    QTemporaryDir dir;
    MediaAsset patches, tone, main;

    Project edit() {
        Project p;
        p.width = 640; p.height = 360; p.fps = 30;
        p.assets = {patches, tone, main};
        Clip v(p.videoTracks[0].id, ClipKind::video, 0, 2); v.assetID = patches.id;
        Clip a(p.audioTracks[0].id, ClipKind::audio, 0, 3); a.assetID = tone.id;
        p.clips = {v, a};
        return p;
    }

    static QString tag(const QString& path, const char* what) {
        AVFormatContext* f = nullptr;
        if (avformat_open_input(&f, path.toUtf8().constData(), nullptr, nullptr) < 0) return {};
        avformat_find_stream_info(f, nullptr);
        QString out;
        for (unsigned i = 0; i < f->nb_streams; ++i) {
            auto* par = f->streams[i]->codecpar;
            if (QByteArray(what) == "video" && par->codec_type == AVMEDIA_TYPE_VIDEO)
                out = QStringLiteral("%1 %2x%3 prim=%4 trc=%5 spc=%6").arg(avcodec_get_name(par->codec_id)).arg(par->width).arg(par->height)
                          .arg(par->color_primaries).arg(par->color_trc).arg(par->color_space);
            if (QByteArray(what) == "audio" && par->codec_type == AVMEDIA_TYPE_AUDIO)
                out = QStringLiteral("%1 %2Hz %3ch %4bit").arg(avcodec_get_name(par->codec_id)).arg(par->sample_rate).arg(par->ch_layout.nb_channels).arg(par->bits_per_coded_sample);
            if (QByteArray(what) == "duration") out = QString::number(double(f->duration) / AV_TIME_BASE, 'f', 3);
        }
        avformat_close_input(&f);
        return out;
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        if (!QFile::exists(clip("sdr_h264.mp4"))) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        patches = *media.importFile(clip("sdr_h264.mp4"));
        tone = *media.importFile(clip("b_tone.wav"));
        main = *media.importFile(clip("a_main.mp4"));
        sound = std::make_unique<SoundStore>(media);
        gpu = GpuRenderer::create(true);
    }

    void settingsMaths() {
        Project p; // 1920×1080
        ExportSettings s;
        QCOMPARE(s.renderSize(p), std::make_pair(1920, 1080));
        s.height = 720;
        QCOMPARE(s.renderSize(p), std::make_pair(1280, 720));
        s.shape = ExportSettings::Shape::vertical;
        QCOMPARE(s.renderSize(p), std::make_pair(720, 1280));
        QVERIFY(s.crops(p));
        s.shape = ExportSettings::Shape::square;
        s.height.reset();
        QCOMPARE(s.renderSize(p), std::make_pair(1080, 1080));
        s.shape = ExportSettings::Shape::portrait;
        QCOMPARE(s.renderSize(p), std::make_pair(1080, 1350));
        s = ExportSettings();
        QCOMPARE(s.videoBitrate(1920, 1080, 30), 12441600LL);
        s.codec = ExportSettings::Codec::hevc;
        s.quality = ExportSettings::Quality::web;
        QCOMPARE(s.videoBitrate(3840, 2160, 120), (long long)(3840.0 * 2160 * 60 * 0.085 * 0.62));
        QCOMPARE(frameDuration(29.97), std::make_pair(1001, 30000));
        QCOMPARE(frameDuration(25), std::make_pair(1, 25));
        QCOMPARE(ExportSettings::label(ExportSettings::Shape::vertical), QStringLiteral("Vertical 9:16"));
    }

    void mp4() {
        const QString out = dir.filePath(QStringLiteral("edit.mp4"));
        std::atomic<bool> cancel{false};
        const auto res = runExport(edit(), media, *sound, gpu.get(), ExportSettings(), out, {}, cancel);
        QVERIFY2(res.ok, qPrintable(res.error));
        qInfo().noquote() << "encoder" << res.encoder << "renderer" << res.renderer << "decode" << res.decode << res.frames << "frames" << res.seconds << "s";
        QCOMPARE(res.frames, 90LL);
        QVERIFY2(tag(out, "video").startsWith(QLatin1String("h264 640x360 prim=1 trc=1 spc=1")), qPrintable(tag(out, "video")));
        QVERIFY2(tag(out, "audio").startsWith(QLatin1String("aac 48000Hz 2ch")), qPrintable(tag(out, "audio")));
        QVERIFY(std::abs(tag(out, "duration").toDouble() - 3.0) < 0.06);
        // The encoded picture against the pre-encode frame.
        BuildOptions o; o.renderW = 640; o.renderH = 360;
        auto plan = *CompositionBuilder::build(edit(), media, o);
        const auto before = CpuRenderer::renderNV12(describeFrame(plan, 1.0, media.provider(640)));
        auto dec = VideoDecoder::open(out, VideoDecoder::Accel::software);
        auto f = dec->frameAt(1.0);
        QVERIFY(f && f->format == ER_SRC_YUV420P);
        std::vector<uint8_t> y(before.begin(), before.begin() + 640 * 360), got(640 * 360);
        for (int r = 0; r < 360; ++r) std::memcpy(got.data() + r * 640, f->data->data() + f->offset[0] + size_t(r) * f->stride[0], 640);
        const double db = test::psnr(y, got, nullptr);
        QVERIFY2(db >= 40, qPrintable(QString::number(db)));
    }

    void hevcAndWav() {
        ExportSettings s;
        s.codec = ExportSettings::Codec::hevc;
        s.height = 180;
        std::atomic<bool> cancel{false};
        const QString out = dir.filePath(QStringLiteral("edit_hevc.mp4"));
        auto res = runExport(edit(), media, *sound, gpu.get(), s, out, {}, cancel);
        QVERIFY2(res.ok, qPrintable(res.error));
        qInfo().noquote() << "HEVC encoder" << res.encoder;
        QVERIFY2(tag(out, "video").startsWith(QLatin1String("hevc 320x180")), qPrintable(tag(out, "video")));
        s = ExportSettings();
        s.format = ExportSettings::Format::wav;
        const QString wav = dir.filePath(QStringLiteral("edit.wav"));
        res = runExport(edit(), media, *sound, gpu.get(), s, wav, {}, cancel);
        QVERIFY2(res.ok, qPrintable(res.error));
        QVERIFY2(tag(wav, "audio").startsWith(QLatin1String("pcm_s24le 48000Hz 2ch")), qPrintable(tag(wav, "audio")));
        // No sound: a WAV export says so.
        Project silent = edit();
        silent.clips.pop_back();
        res = runExport(silent, media, *sound, gpu.get(), s, wav, {}, cancel);
        QVERIFY(!res.ok && res.error == QStringLiteral("There's no audio on the timeline."));
        res = runExport(Project(), media, *sound, gpu.get(), ExportSettings(), out, {}, cancel);
        QCOMPARE(res.error, QStringLiteral("The timeline is empty."));
    }

    // In/out: exporting just the range gives a file the range's length (SelfTest RANGE).
    /// A one-second 640×360 clip whose luma code is `left` on the left half and `right` on the right (10-bit, lossless
    /// FFV1), tagged HLG/Rec.2020 or Rec.709.
    static bool makeClip(const QString& path, bool hlg, int left, int right) {
        QStringList args = {"-y", "-loglevel", "error", "-f", "lavfi", "-i", "nullsrc=s=640x360:r=30:d=1",
                            "-vf", QStringLiteral("format=yuv420p10le,geq=lum='if(lt(X,320),%1,%2)':cb=512:cr=512,"
                                                  "setparams=color_primaries=%3:color_trc=%4:colorspace=%5:range=tv")
                                       .arg(left).arg(right).arg(hlg ? "bt2020" : "bt709", hlg ? "arib-std-b67" : "bt709", hlg ? "bt2020nc" : "bt709"),
                            "-c:v", "ffv1", "-level", "3", path};
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"), args);
        return ff.waitForFinished(60000) && ff.exitCode() == 0 && QFile::exists(path);
    }

    /// Luma (0…1 of the 10-bit video range) at points of the frame at `t`, as the file has it.
    static std::vector<double> luma(const QString& path, double t, const std::vector<std::pair<int, int>>& points) {
        auto dec = VideoDecoder::open(path, VideoDecoder::Accel::software);
        auto f = dec ? dec->frameAt(t) : std::nullopt;
        std::vector<double> out;
        if (!f) return out;
        const uint8_t* b = f->data->data() + f->offset[0];
        for (const auto& [x, y] : points) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(b + size_t(y) * f->stride[0]);
            const int code = f->format == ER_SRC_P010 ? row[x] >> 6 : f->format == ER_SRC_YUV420P10 ? row[x] & 1023 : -1;
            out.push_back((double(code) - 64) / 876);
        }
        return out;
    }

    void hdr() {
        // The Mac's HDR self-test: an HDR (HLG) project keeps an HLG clip's levels (normal white 0.75, a highlight 0.95),
        // an SDR solid and an SDR clip's white land at standard white (0.75, not boosted), and the export is 10-bit HEVC
        // tagged HLG/Rec.2020. The same project as SDR still exports Rec.709.
        const QString hlgPath = dir.filePath(QStringLiteral("hdr_patches.mkv")), sdrPath = dir.filePath(QStringLiteral("sdr_white.mkv"));
        // HLG 0.75 and 0.95 (codes 721, 896); SDR white (940).
        if (!makeClip(hlgPath, true, 721, 896) || !makeClip(sdrPath, false, 940, 940)) QSKIP("ffmpeg couldn't make the test clips");
        auto h = media.importFile(hlgPath), wclip = media.importFile(sdrPath);
        QVERIFY(h && wclip);
        QCOMPARE(SourceColors::name(media.runtime(h->id)->color), SourceColors::name(SourceColor::hlg));
        Project p;
        p.width = 640; p.height = 360; p.fps = 30; p.hdr = true; p.settingsLocked = true;
        p.assets = {*h, *wclip};
        Clip a(p.videoTracks[0].id, ClipKind::video, 0, 1); a.assetID = h->id;
        Clip b(p.videoTracks[0].id, ClipKind::video, 1, 1); b.assetID = wclip->id;
        Clip solid(p.videoTracks[1].id, ClipKind::solid, 0, 1);
        solid.color = RGBA{1, 1, 1, 1};
        ClipTransform t; t.scale = 0.25; t.x = 0; t.y = 0.3;
        solid.transform = t;
        p.clips = {a, b, solid};
        ExportSettings s; s.codec = ExportSettings::Codec::h264; s.includeAudio = false;
        std::atomic<bool> cancel{false};
        for (GpuRenderer* renderer : {static_cast<GpuRenderer*>(nullptr), gpu.get()}) {
            const QString out = dir.filePath(renderer ? QStringLiteral("hdr_gpu.mp4") : QStringLiteral("hdr_cpu.mp4"));
            const auto res = runExport(p, media, *sound, renderer, s, out, {}, cancel);
            QVERIFY2(res.ok, qPrintable(res.error));
            qInfo().noquote() << "HDR encoder" << res.encoder << "renderer" << res.renderer;
            // hevc, BT.2020 (9), ARIB STD-B67 (18), BT.2020 NCL (9).
            QVERIFY2(tag(out, "video").startsWith(QLatin1String("hevc 640x360 prim=9 trc=18 spc=9")), qPrintable(tag(out, "video")));
            const auto first = luma(out, 0.5, {{160, 90}, {480, 90}, {320, 288}});
            const auto second = luma(out, 1.5, {{320, 180}});
            QCOMPARE(first.size(), size_t(3));
            QCOMPARE(second.size(), size_t(1));
            const QString got = QStringLiteral("%1 %2 %3 | %4").arg(first[0], 0, 'f', 3).arg(first[1], 0, 'f', 3).arg(first[2], 0, 'f', 3).arg(second[0], 0, 'f', 3);
            qInfo().noquote() << "HDR levels (0.75 0.95 solid | SDR white)" << got;
            QVERIFY2(std::abs(first[0] - 0.75) < 0.04 && std::abs(first[1] - 0.95) < 0.04, qPrintable(got));
            QVERIFY2(std::abs(first[2] - 0.75) < 0.04 && std::abs(second[0] - 0.75) < 0.04, qPrintable(got));
            if (!gpu) break;
        }
        Project sdr = p;
        sdr.hdr.reset();
        const QString out = dir.filePath(QStringLiteral("hdr_as_sdr.mp4"));
        const auto res = runExport(sdr, media, *sound, gpu.get(), s, out, {}, cancel);
        QVERIFY2(res.ok, qPrintable(res.error));
        QVERIFY2(tag(out, "video").startsWith(QLatin1String("h264 640x360 prim=1 trc=1 spc=1")), qPrintable(tag(out, "video")));
        // In the viewer (no HDR screen), the same HDR project shows SDR white as white, as it does in SDR.
        BuildOptions o; o.renderW = 640; o.renderH = 360;
        auto view = *CompositionBuilder::build(p, media, o);
        QCOMPARE(view.hdr, ER_HDR_VIEW);
        const auto px = CpuRenderer::render(describeFrame(view, 1.5, media.provider(640)));
        const size_t at = (size_t(180) * 640 + 320) * 4;
        QVERIFY2(px[at] >= 253 && px[at + 1] >= 253 && px[at + 2] >= 253, qPrintable(QStringLiteral("%1 %2 %3").arg(px[at]).arg(px[at + 1]).arg(px[at + 2])));
    }

    void range() {
        Project p = edit();
        p.inPoint = 1;
        p.outPoint = 2.5;
        ExportSettings s;
        s.rangeOnly = true;
        s.height = 180;
        std::atomic<bool> cancel{false};
        const QString out = dir.filePath(QStringLiteral("range.mp4"));
        QVERIFY(runExport(p, media, *sound, gpu.get(), s, out, {}, cancel).ok);
        QVERIFY2(std::abs(tag(out, "duration").toDouble() - 1.5) < 0.06, qPrintable(tag(out, "duration")));
    }

    void loudnessTarget() {
        ExportSettings s;
        s.format = ExportSettings::Format::wav;
        s.loudness = -14;
        std::atomic<bool> cancel{false};
        const QString out = dir.filePath(QStringLiteral("loud.wav"));
        QVERIFY(runExport(edit(), media, *sound, gpu.get(), s, out, {}, cancel).ok);
        auto dec = AudioDecoder::open(out, 48000);
        const auto samples = dec->readRange(0, 3);
        auto r = LoudnessMeter::measure(samples);
        QVERIFY(r);
        // The tone peaks at −18 dBFS: −14 LUFS would need more than −1 dBFS of peak, so the peak limit wins.
        QVERIFY2(std::abs(r->peakDB + 1) < 0.1 || std::abs(r->lufs + 14) < 0.3, qPrintable(QString("%1 LUFS, %2 dB").arg(r->lufs).arg(r->peakDB)));
    }

    void cancelled() {
        std::atomic<bool> cancel{true};
        const QString out = dir.filePath(QStringLiteral("cancel.mp4"));
        auto res = runExport(edit(), media, *sound, gpu.get(), ExportSettings(), out, {}, cancel);
        QVERIFY(res.cancelled && !QFile::exists(out));
    }
};

QTEST_MAIN(TestExport)
#include "tst_export.moc"
