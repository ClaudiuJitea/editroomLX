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
