// The compositor on the CPU: what TimelineCompositor.compose does, checked pixel by pixel, including the
// ZOOM "drawn" check and the CAPTIONS drawing check from SelfTest.swift.
#include "../RenderHelpers.h"
#include "rendering/CpuRenderer.h"
#include <QTest>

using namespace er;
using namespace er::test;

class TestRender : public QObject {
    Q_OBJECT

    static std::array<int, 3> px(const std::vector<uint8_t>& img, int w, int x, int y) {
        const size_t i = (size_t(y) * w + x) * 4;
        return {img[i], img[i + 1], img[i + 2]};
    }

    static RenderPlan planFor(const Project& p, const MediaInfo& m, int w, int h, std::optional<std::pair<double, double>> reframe = {}) {
        BuildOptions o;
        o.renderW = w; o.renderH = h; o.reframe = reframe;
        auto plan = CompositionBuilder::build(p, m, o);
        return *plan;
    }

private slots:
    void sourceColor() {
        QVERIFY(!SourceColors::cube(SourceColor::standard));
        auto cube = SourceColors::cube(SourceColor::hlg);
        QVERIFY(cube && cube->size() == size_t(64 * 64 * 64 * 4));
        // HLG's diffuse white (75%) lands near Rec.709 white, eased by the knee.
        auto [r, g, b] = SourceColors::convert(SourceColor::hlg, 0.75, 0.75, 0.75);
        QVERIFY(r > 0.95 && r < 1.0 && std::abs(r - g) < 1e-9 && std::abs(g - b) < 1e-9);
        // iPhone HDR (with Dolby Vision) is compressed as macOS does it (0.317 × light^0.88 + 0.014): diffuse white at
        // about 57% of Rec.709 white, grey staying grey, leaving room for everything brighter.
        auto [vr, vg, vb] = SourceColors::convert(SourceColor::hlgVision, 0.75, 0.75, 0.75);
        QVERIFY2(vr > 0.55 && vr < 0.59 && std::abs(vr - vg) < 1e-9 && std::abs(vg - vb) < 1e-9, qPrintable(QString::number(vr)));
        auto [pr, pg, pb] = SourceColors::convert(SourceColor::hlgVision, 1.0, 1.0, 1.0);
        QVERIFY(pr > vr + 0.1 && pr < 1.0);
        (void)pg; (void)pb;
        QVERIFY(SourceColors::cube(SourceColor::hlgVision) && SourceColors::cube(SourceColor::pqVision));
        // Black stays black; Log's 18% grey is mid-grey.
        auto [k, k2, k3] = SourceColors::convert(SourceColor::pq, 0, 0, 0);
        QVERIFY(k == 0 && k2 == 0 && k3 == 0);
        auto [lg, lg2, lg3] = SourceColors::convert(SourceColor::appleLog, 0.47, 0.47, 0.47);
        Q_UNUSED(lg2); Q_UNUSED(lg3);
        QVERIFY(lg > 0.3 && lg < 0.55);
    }

    void solidsOpacityAndDissolve() {
        Project p;
        const Uuid v = p.videoTracks[0].id;
        Clip a(v, ClipKind::solid, 0, 2); a.color = {1, 0, 0, 1, {}};
        Clip b(v, ClipKind::solid, 2, 2); b.color = {0, 0, 1, 1, {}};
        b.transitionIn = Transition{TransitionKind::dissolve, 1.0, {}};
        Clip c(p.videoTracks[1].id, ClipKind::solid, 0, 4); c.color = {1, 1, 1, 1, {}}; c.fadeIn = 1; c.fadeOut = 0;
        p.clips = {a, b};
        FakeMedia m;
        auto plan = planFor(p, m, 32, 18);
        auto at = [&](double t) { return CpuRenderer::render(describeFrame(plan, t, {})); };
        QCOMPARE(px(at(0.5), 32, 5, 5), (std::array<int, 3>{255, 0, 0}));
        // Solids have no media around the cut: the dissolve sits before it on b's (infinite) head room.
        const auto mid = px(at(1.5), 32, 5, 5);
        QVERIFY2(std::abs(mid[0] - 128) <= 1 && std::abs(mid[2] - 128) <= 1, qPrintable(QString("%1 %2 %3").arg(mid[0]).arg(mid[1]).arg(mid[2])));
        QCOMPARE(px(at(3), 32, 5, 5), (std::array<int, 3>{0, 0, 255}));
        // A fade-in over black at its halfway point.
        p.clips = {c};
        plan = planFor(p, m, 32, 18);
        const auto half = px(CpuRenderer::render(describeFrame(plan, 0.5, {})), 32, 3, 3);
        QVERIFY(std::abs(half[0] - 128) <= 1);
    }

    // Red on the left, blue on the right, zoomed 2× onto the right half: blue at the left edge.
    void zoomDrawn() {
        Project p;
        const Uuid v = p.videoTracks[0].id;
        p.width = 64; p.height = 36;
        MediaAsset img; img.kind = MediaKind::image; img.name = "rb.png"; img.path = "/x/rb.png"; img.width = 64; img.height = 36;
        p.assets = {img};
        Clip pic(v, ClipKind::image, 0, 4); pic.assetID = img.id;
        ClipTransform tr; tr.scale = 2; tr.x = -0.5; tr.y = 0; tr.zoom = ZoomMove::starter(ZoomMove::Mode::zoomIn, 4);
        pic.transform = tr;
        p.clips = {pic};
        FakeMedia m;
        MediaRuntime rt;
        rt.still = picture(64, 36, [](int x, int) { return x < 32 ? std::array<int, 4>{255, 0, 0, 255} : std::array<int, 4>{0, 0, 255, 255}; });
        m.runtimes[img.id] = rt;
        auto plan = planFor(p, m, 64, 36);
        const auto first = px(CpuRenderer::render(describeFrame(plan, 0, {})), 64, 2, 17);
        const auto last = px(CpuRenderer::render(describeFrame(plan, 3, {})), 64, 2, 17);
        QVERIFY(first[0] > 200 && first[2] < 50);
        QVERIFY(last[2] > 200 && last[0] < 50);
    }

    void yuvRanges() {
        // Video-range white and black, and full-range mid grey.
        const int w = 4, h = 2;
        std::vector<uint8_t> y(w * h, 235), uv(w * 2 / 2 * h / 2 * 2, 128);
        const uint8_t* planes[3] = {y.data(), uv.data(), nullptr};
        const int strides[3] = {w, w, 0};
        VideoFrame white = VideoFrame::yuv(ER_SRC_NV12, w, h, planes, strides, ER_MATRIX_709, false);
        std::fill(y.begin(), y.end(), 16);
        VideoFrame black = VideoFrame::yuv(ER_SRC_NV12, w, h, planes, strides, ER_MATRIX_709, false);
        Project p;
        MediaAsset a; a.kind = MediaKind::video; a.duration = 10; a.name = "v"; a.path = "/x/v.mov";
        p.assets = {a};
        p.width = 4; p.height = 2;
        Clip c(p.videoTracks[0].id, ClipKind::video, 0, 5); c.assetID = a.id;
        p.clips = {c};
        FakeMedia m;
        m.runtimes[a.id] = MediaRuntime();
        auto plan = planFor(p, m, 4, 2);
        auto job = describeFrame(plan, 1, [&](int, const PlanLayer&, double) { return white; });
        QCOMPARE(px(CpuRenderer::render(job), 4, 1, 1), (std::array<int, 3>{255, 255, 255}));
        double seen = -1;
        job = describeFrame(plan, 1, [&](int, const PlanLayer&, double s) { seen = s; return std::optional<VideoFrame>(black); });
        QCOMPARE(std::round(seen * 1000) / 1000, 1.0);
        QCOMPARE(px(CpuRenderer::render(job), 4, 1, 1), (std::array<int, 3>{0, 0, 0}));
    }

    void orientation() {
        // A 2×4 stored picture shown with orientation 6 (rotate 90° clockwise): 4×2 on screen.
        auto stored = picture(2, 4, [](int x, int y) { return std::array<int, 4>{x * 100, y * 60, 0, 255}; });
        stored.orientation = 6;
        QCOMPARE(stored.displayWidth(), 4);
        // Display (u, v) ← stored (v, h − 1 − u): display (0, 0) is stored (0, 3).
        const auto p = cm::er_orient(0, 0, 6, 2, 4);
        QCOMPARE(p.x, 0);
        QCOMPARE(p.y, 3);
        const auto q = cm::er_orient(3, 1, 6, 2, 4);
        QVERIFY(q.x == 1 && q.y == 0);
    }

    void reframe() {
        Project p; // 1920×1080
        p.clips = {Clip(p.videoTracks[0].id, ClipKind::solid, 0, 1)};
        FakeMedia m;
        auto plan = planFor(p, m, 1080, 1920, std::make_pair(0.5, 0.5));
        QCOMPARE(plan.canvasH, 1920);
        QCOMPARE(plan.canvasW, 3413);
        QCOMPARE(plan.offsetX, (3413 - 1080) / 2.0);
        QCOMPARE(plan.offsetY, 0.0);
    }

    void gradeChangesPixels() {
        Project p;
        Clip c(p.videoTracks[0].id, ClipKind::solid, 0, 1); c.color = {0.5, 0.5, 0.5, 1, {}};
        ColorGrade g; g.exposure = 1;
        c.grade = g;
        p.clips = {c};
        FakeMedia m;
        auto plan = planFor(p, m, 8, 8);
        const auto v = px(CpuRenderer::render(describeFrame(plan, 0.5, {})), 8, 1, 1);
        // 0.5 × 2 = 1, but the 32³ cube interpolates between lattice points 15 (0.968) and 16 (clamped to 1).
        QVERIFY2(v[0] >= 249 && v[0] <= 252, qPrintable(QString::number(v[0])));
    }

    void text() {
        Project p;
        Clip t(p.videoTracks[0].id, ClipKind::text, 0, 1);
        t.text.string = QStringLiteral("Hello");
        p.clips = {t};
        FakeMedia m;
        auto plan = planFor(p, m, 640, 360);
        const auto img = CpuRenderer::render(describeFrame(plan, 0.5, {}));
        int white = 0, elsewhere = 0;
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x) {
                const auto c = px(img, 640, x, y);
                if (c[0] > 200) (std::abs(y - 180) < 60 ? white : elsewhere)++;
            }
        QVERIFY2(white > 200 && elsewhere == 0, qPrintable(QString("white=%1 elsewhere=%2").arg(white).arg(elsewhere)));
    }

    // Drawn: white text at the captions' height, the word being said in yellow (SelfTest CAPTIONS).
    void captionsDrawn() {
        Project p;
        p.width = 1280; p.height = 720;
        MediaAsset video; video.kind = MediaKind::video; video.duration = 10; video.name = "a"; video.path = "/x/a.mp4"; video.hasAudio = true;
        p.assets = {video};
        Clip ca(p.audioTracks[0].id, ClipKind::audio, 5, 3, 1); ca.assetID = video.id;
        p.clips = {ca};
        p.addCaptions({ca.id});
        FakeMedia m;
        Transcript s;
        for (auto [w, a, b] : std::vector<std::tuple<const char*, double, double>>{{"so", 1.1, 1.3}, {"here's", 1.35, 1.6}, {"the", 1.62, 1.7}, {"thing.", 1.72, 2.0}})
            s.words.push_back({a, b, QString::fromLatin1(w)});
        m.transcripts[video.id] = s;
        auto plan = planFor(p, m, 1280, 720);
        QVERIFY(!plan.captions.empty());
        const auto img = CpuRenderer::render(describeFrame(plan, 5.15, {}));
        int white = 0, yellow = 0, elsewhere = 0;
        for (int y = 0; y < 720; y += 3)
            for (int x = 0; x < 1280; x += 3) {
                const auto c = px(img, 1280, x, y);
                const bool nearY = std::abs(y - 0.78 * 720) < 60;
                if (c[0] > 230 && c[1] > 230 && c[2] > 230) (nearY ? white : elsewhere)++;
                if (nearY && c[0] > 230 && c[1] > 178 && c[2] < 115) yellow++;
            }
        QVERIFY2(white > 40 && yellow > 10 && elsewhere == 0, qPrintable(QString("white=%1 yellow=%2 elsewhere=%3").arg(white).arg(yellow).arg(elsewhere)));
    }

    void nv12Pack() {
        Project p;
        Clip c(p.videoTracks[0].id, ClipKind::solid, 0, 1); c.color = {1, 1, 1, 1, {}};
        p.clips = {c};
        FakeMedia m;
        auto plan = planFor(p, m, 6, 4);
        const auto nv = CpuRenderer::renderNV12(describeFrame(plan, 0.5, {}));
        QCOMPARE(nv.size(), size_t(6 * 4 + 3 * 2 * 2));
        QCOMPARE(int(nv[0]), 235);
        QCOMPARE(int(nv[24]), 128);
    }

    void upscale() {
        // Export enlarges video drawn bigger than its pixels (MetalFX on the Mac, FSR 1's EASU here): the same way up
        // and the same colours as a plain resize, and an edge no softer. 320 × 180: dark left, light right, a red band
        // along the top and a blue one along the bottom; drawn at 1280 × 720.
        FakeMedia m;
        MediaAsset v; v.kind = MediaKind::video; v.duration = 5; v.name = "v"; v.path = "/x/v.mov";
        Project p;
        p.width = 1280; p.height = 720;
        p.assets = {v};
        Clip c(p.videoTracks[0].id, ClipKind::video, 0, 5); c.assetID = v.id;
        p.clips = {c};
        m.runtimes[v.id] = MediaRuntime{};
        const VideoFrame src = picture(320, 180, [](int x, int y) {
            if (y < 30) return std::array<int, 4>{230, 26, 26, 255};
            if (y >= 150) return std::array<int, 4>{26, 26, 230, 255};
            const int g = x < 160 ? 51 : 204;
            return std::array<int, 4>{g, g, g, 255};
        });
        auto frames = [src](int, const PlanLayer&, double) { return std::optional<VideoFrame>(src); };
        auto render = [&](bool up) -> std::vector<uint8_t> {
            BuildOptions o; o.renderW = 1280; o.renderH = 720; o.upscale = up;
            const RenderPlan plan = *CompositionBuilder::build(p, m, o);
            const FrameJob job = describeFrame(plan, 1, frames);
            if (job.layers.at(0).upscale != (up ? 1 : 0)) qFatal("upscale flag not set as asked");
            return CpuRenderer::render(job);
        };
        const auto a = render(true), b = render(false);
        auto px = [](const std::vector<uint8_t>& v, int x, int y) { const size_t i = (size_t(y) * 1280 + x) * 4; return std::array<int, 3>{v[i], v[i + 1], v[i + 2]}; };
        auto near = [](std::array<int, 3> u, std::array<int, 3> w) { return std::abs(u[0] - w[0]) < 14 && std::abs(u[1] - w[1]) < 14 && std::abs(u[2] - w[2]) < 14; };
        for (auto [x, y] : {std::pair{640, 40}, {640, 680}, {300, 360}, {980, 360}}) QVERIFY2(near(px(a, x, y), px(b, x, y)), qPrintable(QStringLiteral("%1,%2").arg(x).arg(y)));
        // The edge: how many pixels across row 360 are partway between dark and light.
        auto soft = [&](const std::vector<uint8_t>& v) { int n = 0; for (int x = 560; x < 720; ++x) { const int r = px(v, x, 360)[0]; if (r > 70 && r < 185) ++n; } return n; };
        qInfo() << "edge pixels: upscaled" << soft(a) << "plain" << soft(b);
        QVERIFY(soft(a) <= soft(b));
        // Never past the four nearest texels: no ringing (nothing darker than dark or lighter than light).
        for (int x = 560; x < 720; ++x) { const int r = px(a, x, 360)[0]; QVERIFY(r >= 50 && r <= 205); }
    }
};

QTEST_MAIN(TestRender)
#include "tst_render.moc"
