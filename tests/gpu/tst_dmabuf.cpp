// DMA-BUF import: frames decoded with VA-API stay on the GPU (exported as DMA-BUFs), the compositor imports them and
// copies them on the GPU into the same layout the CPU copy has. The composite from an imported frame against the
// CPU's from the same frame downloaded, for 8-bit H.264 and 10-bit HEVC; and the decoder's budget of frames out.
// Skipped without VA-API decoding (intel-media-va-driver and the like).
#include "media/Decoder.h"
#include "media/MediaStore.h"
#include "rendering/CpuRenderer.h"
#include "rendering/GpuRenderer.h"
#include <QFile>
#include <QTest>

using namespace er;

class TestDmabuf : public QObject {
    Q_OBJECT
    void compare(const QString& path) {
        if (!QFile::exists(path)) QSKIP("Missing test media");
        auto dec = VideoDecoder::open(path);
        QVERIFY(dec);
        if (dec->path() != QLatin1String("vaapi")) QSKIP("No VA-API decoding here");
        auto f = dec->frameAt(0.5);
        QVERIFY(f);
        QVERIFY2(f->dmabuf, "a VA-API frame should come as a DMA-BUF");
        QVERIFY(!f->data);
        auto gpu = GpuRenderer::create();
        if (!gpu) QSKIP("No GPU");
        MediaStore media;
        auto a = media.importFile(path);
        QVERIFY(a);
        Project p;
        p.width = f->width;
        p.height = f->height;
        p.assets = {*a};
        Clip c(p.videoTracks[0].id, ClipKind::video, 0, 1);
        c.assetID = a->id;
        p.clips = {c};
        BuildOptions o;
        o.renderW = p.width;
        o.renderH = p.height;
        auto plan = CompositionBuilder::build(p, media, o);
        QVERIFY(plan);
        const VideoFrame frame = *f;
        const FrameJob job = describeFrame(*plan, 0.5, [frame](int, const PlanLayer&, double) { return std::optional<VideoFrame>(frame); });
        const auto g = gpu->render(job);      // imported, copied on the GPU
        const auto cpu = CpuRenderer::render(job);   // downloaded
        QCOMPARE(gpu->dmabufImports(), 1LL);
        QVERIFY(frame.dmabuf->downloaded());   // (the CPU's copy, made for the comparison)
        QCOMPARE(g.size(), cpu.size());
        int worst = 0;
        for (size_t i = 0; i < g.size(); ++i) worst = std::max(worst, std::abs(int(g[i]) - int(cpu[i])));
        qInfo() << path.section(u'/', -1) << f->width << "x" << f->height << "worst" << worst;
        QVERIFY(worst <= 1);
    }
private slots:
    void h264() { compare(QStringLiteral(ER_TEST_MEDIA "/a_main.mp4")); }
    void hevc10() { compare(QStringLiteral(ER_TEST_MEDIA "/../../bench-media/4k_hevc10.mp4")); }
    /// Frames out at once are limited (the decoder's surfaces are a fixed pool): past the budget they're downloaded.
    void budget() {
        auto dec = VideoDecoder::open(QStringLiteral(ER_TEST_MEDIA "/a_main.mp4"));
        if (!dec || dec->path() != QLatin1String("vaapi")) QSKIP("No VA-API decoding here");
        std::vector<VideoFrame> kept;
        int gpu = 0, cpu = 0;
        for (int i = 0; i < 60; ++i) {
            auto f = dec->next();
            if (!f) break;
            (f->dmabuf ? gpu : cpu)++;
            kept.push_back(*f);
        }
        qInfo() << "held at once: GPU" << gpu << "memory" << cpu;
        QVERIFY(gpu >= 4);
        QVERIFY(cpu > 0);
        kept.clear();
        auto f = dec->next();
        QVERIFY(f && f->dmabuf);
    }
};

QTEST_MAIN(TestDmabuf)
#include "tst_dmabuf.moc"
