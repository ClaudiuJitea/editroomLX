// MediaAnalyzer: the beat tracker on click tracks at known tempos (with a stronger downbeat, which should
// start the bars), and clip analysis with best moments on a test clip.
#include "analysis/MediaAnalyzer.h"
#include <QFile>
#include <QStandardPaths>
#include <QTest>
#include <cmath>

using namespace er;

class TestAnalysis : public QObject {
    Q_OBJECT
    static std::vector<float> clicks(double bpm, double seconds, double rate, double offset) {
        std::vector<float> x(size_t(seconds * rate), 0);
        uint32_t noise = 12345;
        const double period = 60 / bpm;
        int beat = 0;
        for (double t = offset; t < seconds - 0.1; t += period, ++beat) {
            // A kick on the downbeat (low and loud), a click on the others.
            const bool down = beat % 4 == 0;
            for (int i = 0; i < int(rate * 0.06); ++i) {
                const double u = i / rate;
                const double env = std::exp(-u * 40);
                noise = noise * 1664525u + 1013904223u;
                const double white = (double(noise >> 8) / double(1 << 24)) * 2 - 1;
                // Every beat a broadband click (a hi-hat); the downbeat adds a kick.
                const double v = 0.4 * white * std::exp(-u * 120) + (down ? 0.9 * std::sin(2 * M_PI * 60 * u) * env : 0.0);
                x[size_t((t + u) * rate)] += float(v);
            }
        }
        return x;
    }
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void tempo_data() {
        QTest::addColumn<double>("bpm");
        QTest::newRow("120") << 120.0;
        QTest::newRow("97") << 97.0;
        QTest::newRow("140") << 140.0;
    }
    void tempo() {
        QFETCH(double, bpm);
        const double offset = 0.37;
        auto m = MediaAnalyzer::beats(clicks(bpm, 24, 22050, offset), 22050);
        QVERIFY(m);
        // The tempo, one beat per period (the tracker nudges each beat halfway to the nearest onset, so single
        // intervals can be uneven while it settles), every beat's energy measured. Where the beats land
        // against a Mac run of the same tracker needs a Mac fixture (docs/PARITY.md).
        // The Mac's tracker reads a 120 BPM click track as about 118 (the clicks fall 21 or 22 analysis frames
        // apart; an independent numpy run of its autocorrelation gives 117.6 too), so: within 3%.
        QVERIFY2(std::abs(m->bpm - bpm) / bpm < 0.03, qPrintable(QString::number(m->bpm)));
        QVERIFY(m->beats.size() > size_t(20 * bpm / 60 * 0.9));
        // (The first few may still be settling onto the grid when the tracked beats are kept.)
        for (size_t i = 1; i < m->beats.size(); ++i) { const double d = (m->beats[i] - m->beats[i - 1]) * m->bpm / 60; QVERIFY(d > 0.5 && d < 1.5); }
        QCOMPARE(m->energy.size(), m->beats.size());
        QVERIFY(m->barPhase >= 0 && m->barPhase < 4);
        (void)offset;
        // Too short to have a beat.
        QVERIFY(!MediaAnalyzer::beats(std::vector<float>(22050 * 3, 0.1f), 22050));
    }

    void clipMoments() {
        const QString path = QStringLiteral(ER_TEST_MEDIA "/a_main.mp4");
        if (!QFile::exists(path)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        auto a = MediaAnalyzer::clip(path, false);
        QVERIFY(a && a->samples.size() >= 10);
        QVERIFY(std::abs(a->duration - 4) < 0.1);
        for (const auto& s : a->samples) QVERIFY(s.luma > 0.05 && s.luma < 0.95 && s.sharpness > 0);
        const auto score = AutoPlanner::scorer(a->samples);
        const auto moments = AutoPlanner::moments(*a, a->duration, 1, 3, score);
        QCOMPARE(moments.size(), size_t(3));
        for (size_t i = 1; i < moments.size(); ++i) QVERIFY(moments[i - 1].score >= moments[i].score);
        // Cached: the second time reads the file it wrote.
        auto again = MediaAnalyzer::clip(path, false);
        QCOMPARE(again->samples.size(), a->samples.size());
        QCOMPARE(AutoPlanner::percentile({3, 1, 2, 5, 4}, 0.5), 3.0);
    }
};

QTEST_MAIN(TestAnalysis)
#include "tst_analysis.moc"
