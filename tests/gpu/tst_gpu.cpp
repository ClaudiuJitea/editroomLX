// GPU (Vulkan) vs CPU (OpenMP): the same FrameJobs through both, compared on the 8-bit output
// (max |Δ| ≤ 1 code value and PSNR ≥ 50 dB) and on NV12. Skips cleanly with no Vulkan device.
// CI also runs it on lavapipe (EDITROOM_VK_DEVICE=llvmpipe).
#include "../RenderHelpers.h"
#include "rendering/CpuRenderer.h"
#include "rendering/GpuRenderer.h"
#include <QTest>

using namespace er;
using namespace er::test;

class TestGpu : public QObject {
    Q_OBJECT
    std::unique_ptr<GpuRenderer> gpu;

    struct Case { QString name; RenderPlan plan; double t; FrameProvider frames; };

    std::vector<Case> cases() {
        std::vector<Case> out;
        FakeMedia m;
        auto build = [&](const Project& p, int w, int h, std::optional<std::pair<double, double>> reframe = {}) {
            BuildOptions o; o.renderW = w; o.renderH = h; o.reframe = reframe;
            return *CompositionBuilder::build(p, m, o);
        };
        MediaAsset v; v.kind = MediaKind::video; v.duration = 10; v.name = "v"; v.path = "/x/v.mov";
        const VideoFrame nv12 = gradientYuv(320, 180, 8);
        const VideoFrame p010 = gradientYuv(321, 181, 10, ER_MATRIX_2020);
        VideoFrame rotated = gradientYuv(90, 160, 8);
        auto still = picture(300, 200, [](int x, int y) { return std::array<int, 4>{(x * 7) % 256, (y * 5) % 256, (x + y) % 256, 255}; });
        auto translucent = picture(64, 64, [](int x, int y) { const int a = (x * 4) % 256; return std::array<int, 4>{a * ((y * 3) % 256) / 255, a / 2, 0, a}; });

        for (int kind = 0; kind < 4; ++kind) {
            Project p;
            p.assets = {v};
            Clip c(p.videoTracks[0].id, ClipKind::video, 0, 5); c.assetID = v.id;
            Clip d(p.videoTracks[0].id, ClipKind::video, 5, 5, 3); d.assetID = v.id;
            d.transitionIn = Transition{kind == 1 ? TransitionKind::wipe : TransitionKind::dissolve, 1.0, {}};
            ColorGrade g; g.look = QStringLiteral("Film print"); g.lookIntensity = 0.7; g.contrast = 10; g.midtones = {0.2, -0.3, {}};
            if (kind >= 2) c.grade = g;
            ClipTransform tr; tr.scale = 1.37; tr.x = 0.11; tr.y = -0.07;
            if (kind == 3) d.transform = tr;
            Clip text(p.videoTracks[1].id, ClipKind::text, 0, 10);
            text.text.string = QStringLiteral("Grüße — 4K");
            p.clips = {c, d, text};
            MediaRuntime rt;
            rt.color = kind == 2 ? SourceColor::hlg : kind == 3 ? SourceColor::appleLog : SourceColor::standard;
            rt.orientation = 1;
            m.runtimes[v.id] = rt;
            const VideoFrame src = kind == 2 ? p010 : nv12;
            auto frames = [src](int, const PlanLayer&, double) { return std::optional<VideoFrame>(src); };
            out.push_back({QStringLiteral("video-%1").arg(kind), build(p, 256, 144), 4.6, frames});
        }
        {
            // A rotated phone video, a still and a half-transparent still over it, fading, reframed to 9:16
            // around an off-centre point (a window between pixels).
            Project p;
            MediaAsset im; im.kind = MediaKind::image; im.name = "im"; im.path = "/x/im.png";
            MediaAsset tl; tl.kind = MediaKind::image; tl.name = "tl"; tl.path = "/x/tl.png";
            p.assets = {v, im, tl};
            Clip c(p.videoTracks[0].id, ClipKind::video, 0, 5); c.assetID = v.id; c.fadeIn = 2; c.fadeInCurve = FadeCurve::smooth;
            Clip s(p.videoTracks[1].id, ClipKind::image, 0, 5); s.assetID = im.id; s.fadeOut = 3;
            ClipTransform tr; tr.scale = 0.6; tr.x = 0.2; tr.y = 0.15;
            s.transform = tr;
            Clip t(p.videoTracks[1].id, ClipKind::image, 5, 5); t.assetID = tl.id;
            p.clips = {c, s, t};
            MediaRuntime rv; rv.orientation = 6; m.runtimes[v.id] = rv;
            MediaRuntime ri; ri.still = still; m.runtimes[im.id] = ri;
            MediaRuntime rt2; rt2.still = translucent; m.runtimes[tl.id] = rt2;
            auto frames = [rotated](int, const PlanLayer&, double) { return std::optional<VideoFrame>(rotated); };
            out.push_back({QStringLiteral("stills-reframe"), build(p, 144, 256, std::make_pair(0.37, 0.5)), 1.3, frames});
        }
        {
            Project p;
            p.width = 1280; p.height = 720;
            MediaAsset a; a.kind = MediaKind::video; a.duration = 10; a.name = "a"; a.path = "/x/a.mp4"; a.hasAudio = true;
            p.assets = {a};
            Clip sol(p.videoTracks[0].id, ClipKind::solid, 0, 10); sol.color = {0.2, 0.6, 0.3, 1, {}};
            Clip ca(p.audioTracks[0].id, ClipKind::audio, 0, 8); ca.assetID = a.id;
            p.clips = {sol, ca};
            p.addCaptions({ca.id});
            Transcript tr;
            tr.words = {{0.1, 0.4, "we"}, {0.45, 0.8, "made"}, {0.85, 1.2, "it!"}};
            m.transcripts[a.id] = tr;
            out.push_back({QStringLiteral("captions"), build(p, 640, 360), 0.5, {}});
        }
        {
            // Export's upscaler (EASU): a small video enlarged into a bigger frame, under a transform.
            Project p;
            p.assets = {v};
            Clip c(p.videoTracks[0].id, ClipKind::video, 0, 5); c.assetID = v.id;
            ClipTransform tr; tr.scale = 1.2; tr.x = 0.05;
            c.transform = tr;
            p.clips = {c};
            MediaRuntime rt; m.runtimes[v.id] = rt;
            auto frames = [nv12](int, const PlanLayer&, double) { return std::optional<VideoFrame>(nv12); };
            BuildOptions o; o.renderW = 1280; o.renderH = 720; o.upscale = true;
            out.push_back({QStringLiteral("upscale"), *CompositionBuilder::build(p, m, o), 1.0, frames});
        }
        for (bool hlgOut : {true, false}) {
            // An HDR project: an HLG clip, graded, dissolving into an SDR one, a title and a half-faded solid over them;
            // as HLG for the export, and as SDR for the viewer.
            Project p;
            p.hdr = true;
            MediaAsset h; h.kind = MediaKind::video; h.duration = 10; h.name = "h"; h.path = "/x/h.mov";
            p.assets = {v, h};
            Clip c(p.videoTracks[0].id, ClipKind::video, 0, 5); c.assetID = h.id;
            ColorGrade g; g.contrast = 12; g.highlightsLevel = -30; g.midtones = {0.1, 0.2, {}};
            c.grade = g;
            Clip d(p.videoTracks[0].id, ClipKind::video, 5, 5, 3); d.assetID = v.id;
            d.transitionIn = Transition{TransitionKind::dissolve, 1.0, {}};
            Clip solid(p.videoTracks[1].id, ClipKind::solid, 4, 2); solid.color = RGBA{0.9, 0.3, 0.1, 1}; solid.fadeIn = 1.2;
            ClipTransform tr; tr.scale = 0.4; tr.x = 0.2; solid.transform = tr;
            Clip text(p.videoTracks[1].id, ClipKind::text, 0, 4);
            text.text.string = QStringLiteral("HDR — HLG");
            p.clips = {c, d, solid, text};
            MediaRuntime hr; hr.color = SourceColor::hlg; m.runtimes[h.id] = hr;
            MediaRuntime vr; m.runtimes[v.id] = vr;
            const VideoFrame hf = p010, sf = nv12;
            const Uuid hid = h.id;
            auto frames = [hf, sf, hid](int, const PlanLayer& l, double) { return std::optional<VideoFrame>(l.asset == hid ? hf : sf); };
            BuildOptions o; o.renderW = 320; o.renderH = 180; o.hdrOutput = hlgOut;
            out.push_back({hlgOut ? QStringLiteral("hdr-hlg") : QStringLiteral("hdr-view"), *CompositionBuilder::build(p, m, o), 4.6, frames});
        }
        return out;
    }

private slots:
    void initTestCase() {
        QString why;
        gpu = GpuRenderer::create(true, &why);
        if (!gpu) QSKIP(qPrintable(QStringLiteral("No Vulkan device: ") + why));
        qInfo() << "GPU:" << gpu->info().device << gpu->info().driver << "software:" << gpu->info().software;
    }

    void equivalence() {
        for (const auto& c : cases()) {
            const FrameJob job = describeFrame(c.plan, c.t, c.frames);
            QVERIFY2(job.frame.layerCount + job.frame.captionCount > 0, qPrintable(c.name));
            const auto a = gpu->render(job);
            const auto b = CpuRenderer::render(job);
            QCOMPARE(a.size(), b.size());
            int md = 0;
            const double db = psnr(a, b, &md);
            qInfo().noquote() << QStringLiteral("%1: max |Δ| %2, PSNR %3 dB, GPU %4 ms").arg(c.name).arg(md).arg(db, 0, 'f', 1).arg(gpu->lastGpuMs(), 0, 'f', 3);
            QVERIFY2(md <= 1 && db >= 50, qPrintable(QStringLiteral("%1: max %2, %3 dB").arg(c.name).arg(md).arg(db)));
            const auto na = gpu->renderNV12(job);
            const auto nb = CpuRenderer::renderNV12(job);
            QCOMPARE(na.size(), nb.size());
            const double ndb = psnr(na, nb, &md);
            QVERIFY2(md <= 1 && ndb >= 50, qPrintable(QStringLiteral("%1 NV12: max %2, %3 dB").arg(c.name).arg(md).arg(ndb)));
            if (c.plan.hdr == ER_HDR_HLG) {
                // The HDR export's 10-bit frames: packed on the GPU, against the CPU's.
                const auto pa = gpu->renderP010(job);
                const auto pb = CpuRenderer::renderP010(job);
                QCOMPARE(pa.size(), pb.size());
                int worst = 0;
                for (size_t i = 0; i < pa.size(); ++i) worst = std::max(worst, std::abs(int(pa[i] >> 6) - int(pb[i] >> 6)));
                qInfo().noquote() << QStringLiteral("%1 P010: max |Δ| %2 (10-bit codes)").arg(c.name).arg(worst);
                QVERIFY2(worst <= 1, qPrintable(QStringLiteral("%1 P010: max %2").arg(c.name).arg(worst)));
            }
        }
    }

    void reusesUploads() {
        // The same job twice: the second skips copying pictures and gives the same pixels.
        const auto cs = cases();
        const FrameJob job = describeFrame(cs[0].plan, cs[0].t, cs[0].frames);
        const auto a = gpu->render(job);
        const auto b = gpu->render(job);
        QVERIFY(a == b);
    }
};

QTEST_MAIN(TestGpu)
#include "tst_gpu.moc"
