// The viewer presenting straight from the GPU: on X11 or Wayland a frame goes into the Vulkan
// window over the picture with nothing read back, and painting takes over while a library item is previewed.
// Skipped offscreen (no window system to present to).
#include "ui/Controller.h"
#include "ui/PreviewPanel.h"
#include "ui/MainWindow.h"
#include "ui/Workspace.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QElapsedTimer>
#include "rendering/RenderPlan.h"
#include <cstring>
#include <QFile>
#include <QScreen>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace er;

class TestPresent : public QObject {
    Q_OBJECT
private slots:
    void presents() {
        const QString platform = QGuiApplication::platformName();
        if (platform != QLatin1String("xcb") && platform != QLatin1String("wayland")) QSKIP("Needs a display (X11 or Wayland)");
        // QtTest sets this, and then Qt's Wayland windows attach a placeholder shared-memory buffer on expose
        // (a workaround for its own tests); on a Vulkan surface with explicit sync that's a protocol error. The app
        // never runs with it, so neither does this test.
        qunsetenv("QT_QTESTLIB_RUNNING");
        QStandardPaths::setTestModeEnabled(true);
        const QString path = QStringLiteral(ER_TEST_MEDIA "/a_main.mp4");
        if (!QFile::exists(path)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Theme::loadFonts();
        Controller c;
        if (!c.engine().presentInstance()) QSKIP("No Vulkan device that can present");
        auto a = c.media().importFile(path);
        c.model().changeLibrary([&](Project& p) { p.assets.push_back(*a); });
        Clip v(c.model().project().videoTracks[0].id, ClipKind::video, 0, 4);
        v.assetID = a->id;
        c.model().mutate([&](Project& p) { p.clips = {v}; });
        PreviewPanel panel(c);
        panel.resize(900, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QSignalSpy presented(&c.engine(), &PlaybackEngine::framePresented);
        QTRY_VERIFY_WITH_TIMEOUT(panel.viewer()->presenting(), 5000);
        c.engine().seek(1.0);
        QVERIFY(presented.wait(5000));
        // What the window shows is the frame, colours and all: the presented image against the same frame read back.
        c.engine().seek(1.5);
        QTRY_VERIFY_WITH_TIMEOUT(!presented.isEmpty() && std::abs(presented.last().at(0).toDouble() - 1.5) < 1e-6, 5000);
        // The same frame again, captured as it's presented.
        c.engine().gpu()->capturePresented();
        c.engine().redraw();
        QTRY_VERIFY_WITH_TIMEOUT(!c.engine().gpu()->presentedImage().isNull(), 5000);
        const QImage shown = c.engine().gpu()->presentedImage();
        if (const QString dir = qEnvironmentVariable("ER_SHOTS"); !dir.isEmpty()) shown.save(dir + QStringLiteral("/present.png"));
        c.engine().setPresenting(false);
        QSignalSpy back(&c.engine(), &PlaybackEngine::frameReady);
        c.engine().seek(1.5);
        QVERIFY(back.wait(5000));
        const QImage ref = back.first().at(0).value<QImage>().scaled(shown.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        int worst = 0;
        for (int i = 1; i < 10; ++i) {
            const QPoint p(shown.width() * i / 10, shown.height() * i / 10);
            const QRgb a = shown.pixel(p), b = ref.pixel(p);
            worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
        }
        qInfo() << "presented against read back: worst" << worst;
        QVERIFY(worst <= 24);
        c.engine().setPresenting(true);
        QTRY_VERIFY(panel.viewer()->presenting());
        // Playing: every frame presented, none read back. (First let any frame read back for the steps above arrive.)
        QTest::qWait(300);
        QSignalSpy painted(&c.engine(), &PlaybackEngine::frameReady);
        c.engine().play();
        QTest::qWait(1000);
        c.engine().pause();
        qInfo() << "presented" << presented.count() << "read back" << painted.count();

        QVERIFY(presented.count() > 10);
        QCOMPARE(painted.count(), 0);
        qInfo("step resize");
        // Resized: the swapchain follows.
        panel.resize(700, 500);
        QTest::qWait(200);
        c.engine().seek(2.0);
        QVERIFY(presented.wait(5000));
        qInfo("step off");
        // Turned off: frames are read back again.
        c.engine().setPresenting(false);
        QVERIFY(painted.wait(5000));
        qInfo("step end");
        // Timings: a 1920x1080 frame presented, against read back and made into an image (what painting needs).
        Project big = c.model().project();
        big.width = 1920;
        big.height = 1080;
        BuildOptions o;
        o.renderW = 1920;
        o.renderH = 1080;
        auto plan = CompositionBuilder::build(big, c.media(), o);
        QVERIFY(plan);
        const FrameJob job = describeFrame(*plan, 1.0, c.media().provider(1920));
        GpuRenderer* gpu = c.engine().gpu();
        auto timeIt = [](const std::function<void()>& f) {
            QElapsedTimer t;
            std::vector<double> ms;
            for (int i = 0; i < 15; ++i) { t.start(); f(); ms.push_back(t.nsecsElapsed() / 1e6); }
            std::sort(ms.begin(), ms.end());
            return ms[ms.size() / 2];
        };
        const double readBack = timeIt([&] {
            const auto px = gpu->render(job);
            QImage img(1920, 1080, QImage::Format_RGBX8888);
            for (int y = 0; y < 1080; ++y) std::memcpy(img.scanLine(y), px.data() + size_t(y) * 1920 * 4, 1920 * 4);
        });
        const double presentMs = timeIt([&] { gpu->present(job); });
        qInfo().noquote() << QStringLiteral("1920x1080: read back + image %1 ms, presented %2 ms").arg(readBack, 0, 'f', 2).arg(presentMs, 0, 'f', 2);
    }

    void presentsZoomed() {
        // Zoomed in (200%), the viewer still presents, in a window over just the part in view, showing that part of the
        // frame (the Mac's Metal layer is scaled; LX blits the part in view).
        const QString platform = QGuiApplication::platformName();
        if (platform != QLatin1String("xcb") && platform != QLatin1String("wayland")) QSKIP("Needs a display (X11 or Wayland)");
        qunsetenv("QT_QTESTLIB_RUNNING");
        const QString path = QStringLiteral(ER_TEST_MEDIA "/sdr_h264.mp4");
        if (!QFile::exists(path)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Controller c;
        if (!c.engine().presentInstance()) QSKIP("No Vulkan device that can present");
        auto a = c.media().importFile(path);
        c.model().changeLibrary([&](Project& p) { p.assets.push_back(*a); p.width = 640; p.height = 360; });
        Clip v(c.model().project().videoTracks[0].id, ClipKind::video, 0, 2);
        v.assetID = a->id;
        c.model().mutate([&](Project& p) { p.clips = {v}; });
        PreviewPanel panel(c);
        panel.resize(700, 520);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QTRY_VERIFY_WITH_TIMEOUT(panel.viewer()->presenting(), 5000);
        panel.setZoom(2.0);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(panel.viewer()->presenting(), 5000);
        // Bigger than the view: it scrolls, and only what's in view is presented.
        const QRectF pic = panel.viewer()->picture();
        const QRect inView = panel.viewer()->visibleRegion().boundingRect();
        QVERIFY(pic.width() > inView.width());
        c.engine().gpu()->capturePresented();
        c.engine().redraw();
        QTRY_VERIFY_WITH_TIMEOUT(!c.engine().gpu()->presentedImage().isNull(), 5000);
        const QImage shown = c.engine().gpu()->presentedImage();
        // The same frame read back, cut to what's in view, against what was presented.
        c.engine().setPresenting(false);
        QSignalSpy back(&c.engine(), &PlaybackEngine::frameReady);
        c.engine().redraw();
        QVERIFY(back.wait(5000));
        const QImage frame = back.last().at(0).value<QImage>();
        const QRectF part = pic.intersected(QRectF(inView));
        const QRect crop(int((part.left() - pic.left()) / pic.width() * frame.width()), int((part.top() - pic.top()) / pic.height() * frame.height()),
                         int(part.width() / pic.width() * frame.width()), int(part.height() / pic.height() * frame.height()));
        const QImage ref = frame.copy(crop).scaled(shown.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        int worst = 0;
        for (int i = 1; i < 10; ++i) {
            const QPoint p(shown.width() * i / 10, shown.height() * i / 10);
            const QRgb x = shown.pixel(p), y = ref.pixel(p);
            worst = std::max({worst, std::abs(qRed(x) - qRed(y)), std::abs(qGreen(x) - qGreen(y)), std::abs(qBlue(x) - qBlue(y))});
        }
        qInfo() << "zoomed: presented" << shown.size() << "of" << frame.size() << "crop" << crop << "worst" << worst;
        QVERIFY(worst <= 24);
    }

    void presentsAfterSwitchingTabs() {
        // Tabs (1.1): every tab's player shares one GPU, and the window surface goes to the viewer in front. Switching
        // away and back, the viewer in front still presents; the tab behind has let go of its player.
        const QString platform = QGuiApplication::platformName();
        if (platform != QLatin1String("xcb") && platform != QLatin1String("wayland")) QSKIP("Needs a display (X11 or Wayland)");
        qunsetenv("QT_QTESTLIB_RUNNING");
        const QString path = QStringLiteral(ER_TEST_MEDIA "/a_main.mp4");
        if (!QFile::exists(path)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Workspace w(false);
        MainWindow win(w);
        win.resize(1480, 940);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        auto load = [&](Controller& c) {
            auto a = c.media().importFile(path);
            c.model().changeLibrary([&](Project& p) { p.assets.push_back(*a); });
            Clip v(c.model().project().videoTracks[0].id, ClipKind::video, 0, 4);
            v.assetID = a->id;
            c.model().mutate([&](Project& p) { p.clips = {v}; });
        };
        load(win.controller());
        if (!win.controller().engine().presentInstance()) QSKIP("No Vulkan device that can present");
        auto viewer = [&] { return win.findChild<PreviewPanel*>()->viewer(); };
        QTRY_VERIFY_WITH_TIMEOUT(viewer()->presenting(), 5000);
        Controller* first = &win.controller();
        w.newTab();
        load(win.controller());
        QTRY_VERIFY_WITH_TIMEOUT(viewer()->presenting(), 5000);
        QVERIFY(first->isBackground() && first->engine().isEmpty());
        for (int round = 0; round < 3; ++round) {
            w.select(round % 2 == 0 ? 0 : 1);
            QTRY_VERIFY_WITH_TIMEOUT(viewer()->presenting(), 5000);
            QSignalSpy presented(&win.controller().engine(), &PlaybackEngine::framePresented);
            win.controller().engine().seek(1.0 + round * 0.5);
            QVERIFY2(presented.wait(5000), qPrintable(QStringLiteral("round %1").arg(round)));
        }
    }
};

QTEST_MAIN(TestPresent)
#include "tst_present.moc"
