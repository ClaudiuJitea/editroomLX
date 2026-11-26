// Made-ahead scrub frames: twelve a second, the file's own colours (as an exact decode, within JPEG's
// rounding), kept on disk and read back as mapped, quicker than a seek, and used by the player while scrubbing.
#include "engine/ScrubCache.h"
#include "media/Decoder.h"
#include "ui/Controller.h"
#include "ui/Thumbnails.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QTest>

using namespace er;

class TestScrub : public QObject {
    Q_OBJECT
    QString path = QStringLiteral(ER_TEST_MEDIA "/a_main.mp4");
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        if (!QFile::exists(path)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        QDir(ScrubCache::folder()).removeRecursively();
    }

    void frames() {
        ScrubCache cache;
        const Uuid id = newUuid();
        cache.make(id, path, 4.0);
        cache.waitForDone();
        QVERIFY(cache.has(id));
        auto f = cache.frame(id, 2.0);
        QVERIFY(f);
        // Against the exact frame at 2 s, through the same conversion: within a few codes (JPEG).
        auto dec = VideoDecoder::open(path, VideoDecoder::Accel::software);
        auto exact = dec->frameAt(2.0);
        QVERIFY(exact);
        const QImage a = Thumbnails::fit(*f, SourceColor::standard, 320), b = Thumbnails::fit(*exact, SourceColor::standard, 320);
        QCOMPARE(a.size(), b.size());
        double err = 0;
        int n = 0;
        for (int y = 8; y < a.height() - 8; y += 7)
            for (int x = 8; x < a.width() - 8; x += 7) {
                const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                err += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
                n += 3;
            }
        QVERIFY2(err / n < 6, qPrintable(QString::number(err / n)));
        // Kept on disk: a new cache has them at once.
        ScrubCache again;
        again.make(id, path, 4.0);
        QElapsedTimer t;
        t.start();
        again.waitForDone();
        QVERIFY(again.has(id));
        QVERIFY(t.elapsed() < 2000);
    }

    /// What they're for: long-GOP footage, where an exact seek decodes from a keyframe far back.
    void quickerThanSeeking() {
        QTemporaryDir dir;
        const QString longGop = dir.filePath(QStringLiteral("longgop.mp4"));
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"), {"-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=8",
                                            "-c:v", "libx264", "-preset", "ultrafast", "-g", "240", "-pix_fmt", "yuv420p", longGop});
        if (!ff.waitForFinished(120000) || ff.exitCode() != 0) QSKIP("ffmpeg couldn't make the long-GOP clip");
        ScrubCache cache;
        const Uuid id = newUuid();
        cache.make(id, longGop, 8.0);
        cache.waitForDone();
        QVERIFY(cache.has(id));
        auto dec = VideoDecoder::open(longGop, VideoDecoder::Accel::software);
        // Scrubbing jumps about: back and forth across the file.
        const std::vector<double> times = {7.3, 2.1, 6.8, 4.4, 7.6, 1.2, 5.6, 3.3};
        QElapsedTimer t;
        t.start();
        for (double x : times) QVERIFY(cache.frame(id, x));
        const double cached = t.nsecsElapsed() / 1e6 / times.size();
        t.restart();
        for (double x : times) QVERIFY(dec->frameAt(x));
        const double seeking = t.nsecsElapsed() / 1e6 / times.size();
        qInfo() << "scrub frame" << cached << "ms, exact seek" << seeking << "ms";
        QVERIFY(cached < seeking);
    }

    void playerUsesThem() {
        Controller c;
        auto a = c.media().importFile(path);
        c.model().changeLibrary([&](Project& p) { p.assets.push_back(*a); });
        Clip v(c.model().project().videoTracks[0].id, ClipKind::video, 0, 4);
        v.assetID = a->id;
        c.model().mutate([&](Project& p) { p.clips = {v}; });
        c.model().flushRebuild();
        c.scrubCache().waitForDone();
        QVERIFY(c.scrubCache().has(a->id));
        QVERIFY(c.showScrubFrame(1.5));
    }
};

QTEST_MAIN(TestScrub)
#include "tst_scrub.moc"
