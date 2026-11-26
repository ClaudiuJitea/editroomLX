// Media: probing and decoding the test clips (scripts/make-test-clips.py --set tests/media), and the
// FormatCheck patch colours through the whole pipeline (decode → plan → CPU and GPU compositor).
#include "../RenderHelpers.h"
#include "media/MediaStore.h"
#include "media/Stills.h"
#include "rendering/CpuRenderer.h"
#include "rendering/GpuRenderer.h"
#include <QFile>
#include <QTest>
#include <cmath>

using namespace er;

class TestMedia : public QObject {
    Q_OBJECT
    static QString clip(const char* name) { return QStringLiteral(ER_TEST_MEDIA "/") + QLatin1String(name); }

    /// What an unmanaged Rec.709 pipeline should show for each patch: the sRGB colour's light, encoded
    /// with the Rec.709 curve (highlights above 0.9 eased as SourceColor does for HDR and Log).
    static std::array<int, 3> ideal(std::array<int, 3> srgb, bool hdr) {
        auto lin = [](int v) { const double c = v / 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        double c[3] = {lin(srgb[0]), lin(srgb[1]), lin(srgb[2])};
        const double peak = std::max({c[0], c[1], c[2]});
        if (hdr && peak > 0.9) { const double eased = 0.9 + 0.1 * (1 - std::exp(-(peak - 0.9) / 0.1)); for (double& x : c) x = x * eased / peak; }
        std::array<int, 3> out;
        for (int i = 0; i < 3; ++i) {
            const double v = std::min(1.0, std::max(0.0, c[i]));
            out[size_t(i)] = int(std::round((v < 0.018 ? 4.5 * v : 1.099 * std::pow(v, 0.45) - 0.099) * 255));
        }
        return out;
    }

private slots:
    void initTestCase() {
        if (!QFile::exists(clip("sdr_h264.mp4"))) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
    }

    void probe() {
        auto sdr = Probe::file(clip("sdr_h264.mp4"));
        QVERIFY(sdr && sdr->hasVideo && !sdr->hasAudio);
        QCOMPARE(sdr->width, 640);
        QCOMPARE(sdr->height, 360);
        QVERIFY(std::abs(sdr->fps - 30) < 1e-6);
        QVERIFY(std::abs(sdr->duration - 3) < 0.05);
        QCOMPARE(sdr->color, SourceColor::standard);
        QCOMPARE(Probe::file(clip("hlg_hevc10.mov"))->color, SourceColor::hlg);
        QCOMPARE(Probe::file(clip("pq_hevc10.mov"))->color, SourceColor::pq);
        auto rot = Probe::file(clip("rotated_h264.mp4"));
        QVERIFY(rot);
        // Turned a quarter: 240×320 on screen.
        QCOMPARE(rot->width, 240);
        QCOMPARE(rot->height, 320);
        QVERIFY(rot->orientation == 6 || rot->orientation == 8);
        auto tone = Probe::file(clip("b_tone.wav"));
        QVERIFY(tone && !tone->hasVideo && tone->hasAudio && tone->audioRate == 48000);
        QVERIFY(!Probe::file(clip("nope.mp4")));
    }

    void decode() {
        auto dec = VideoDecoder::open(clip("sdr_h264.mp4"));
        QVERIFY(dec);
        auto f = dec->frameAt(1.0);
        qInfo() << "decode path:" << dec->path();
        QVERIFY(f && f->valid());
        QVERIFY(std::abs(f->time - 1.0) < 1.0 / 30 + 1e-6);
        QVERIFY(dec->seek(2.0));
        auto g = dec->frameAt(2.5);
        QVERIFY(g && std::abs(g->time - 2.5) < 1.0 / 30 + 1e-6);
        // Software path too.
        auto sw = VideoDecoder::open(clip("hlg_hevc10.mov"), VideoDecoder::Accel::software);
        QVERIFY(sw);
        auto h = sw->frameAt(0.5);
        QVERIFY(h && (h->format == ER_SRC_P010 || h->format == ER_SRC_YUV420P10));
    }

    void chunks() {
        auto cache = ChunkCache::create(clip("sdr_h264.mp4"));
        // Forwards across chunk edges, then backwards.
        for (double t = 0; t < 2.9; t += 0.1) {
            auto f = cache->frameAt(t, 640);
            QVERIFY2(f && f->time <= t + 1e-6 && t - f->time < 1.0 / 30 + 1e-6, qPrintable(QString::number(t)));
        }
        for (double t = 2.9; t > 0.1; t -= 0.13) {
            auto f = cache->frameBefore(t, 640);
            QVERIFY2(f && f->time < t && t - f->time <= 1.0 / 30 + 1e-6, qPrintable(QString::number(t)));
        }
    }

    void audio() {
        auto a = AudioDecoder::open(clip("b_tone.wav"), 48000);
        QVERIFY(a);
        const auto s = a->readRange(1.0, 1.5);
        QCOMPARE(int(s.size()), 48000);
        double rms = 0;
        for (float v : s) rms += double(v) * v;
        rms = std::sqrt(rms / s.size());
        // lavfi sine: −24.08 dB RMS (ffmpeg astats).
        QVERIFY2(std::abs(20 * std::log10(rms) + 24.08) < 0.1, qPrintable(QString::number(rms)));
    }

    void stills() {
        auto sz = Stills::size(clip("photo.jpg"));
        QVERIFY(sz && sz->first == 600 && sz->second == 400);
        auto f = Stills::frame(clip("photo.jpg"));
        QVERIFY(f && f->width == 600);
    }

    // The FormatCheck patches through the whole pipeline: worst channel within 4 codes of the ideal.
    void formatCheck() {
        std::unique_ptr<GpuRenderer> gpu = GpuRenderer::create(true);
        const std::array<std::array<int, 3>, 6> expected{{{118, 118, 118}, {235, 235, 235}, {200, 40, 40}, {40, 170, 60}, {40, 60, 200}, {224, 172, 105}}};
        for (const char* name : {"sdr_h264.mp4", "hlg_hevc10.mov", "pq_hevc10.mov", "applelog_hevc10.mov"}) {
            MediaStore media;
            auto a = media.importFile(clip(name));
            QVERIFY2(a, name);
            const bool hdr = QByteArray(name).startsWith("hlg") || QByteArray(name).startsWith("pq") || QByteArray(name).startsWith("applelog");
            Project p;
            p.width = a->width; p.height = a->height; p.fps = 30;
            p.assets = {*a};
            Clip c(p.videoTracks[0].id, ClipKind::video, 0, std::min(a->duration, 1.5)); c.assetID = a->id;
            p.clips = {c};
            BuildOptions o; o.renderW = p.width; o.renderH = p.height;
            auto plan = *CompositionBuilder::build(p, media, o);
            const FrameJob job = describeFrame(plan, 0.5, media.provider(std::max(p.width, p.height)));
            QVERIFY2(job.frame.layerCount == 1, name);
            for (int pass = 0; pass < (gpu ? 2 : 1); ++pass) {
                const auto img = pass == 0 ? CpuRenderer::render(job) : gpu->render(job);
                int worst = 0;
                QStringList got;
                for (int i = 0; i < 6; ++i) {
                    const int x = (2 * i + 1) * p.width / 12, y = p.height * 3 / 10;
                    const size_t o2 = (size_t(y) * p.width + x) * 4;
                    const auto want = ideal(expected[size_t(i)], hdr);
                    for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(int(img[o2 + k]) - want[size_t(k)]));
                    got << QStringLiteral("%1,%2,%3").arg(img[o2]).arg(img[o2 + 1]).arg(img[o2 + 2]);
                }
                qInfo().noquote() << QStringLiteral("FORMAT %1 (%2, %3): worst off by %4 [%5]").arg(QLatin1String(name), pass ? "GPU" : "CPU",
                    media.decodePath(a->id)).arg(worst).arg(got.join(QStringLiteral("  ")));
                QVERIFY2(worst <= 4, name);
            }
        }
    }
};

QTEST_MAIN(TestMedia)
#include "tst_media.moc"
