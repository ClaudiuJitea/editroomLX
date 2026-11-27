// Audio: the mix plan and mixer against what the Mac's AVAudioMix does (gains, fades, ducking, speed,
// reverse), BS.1770 against EBU Tech 3341, waveforms, and the stream's clock.
#include "audio/AudioStream.h"
#include "audio/Loudness.h"
#include "audio/Mixer.h"
#include "audio/ReverseAudio.h"
#include "audio/Waveform.h"
#include "media/MediaStore.h"
#include <QFile>
#include <QStandardPaths>
#include <QTest>
#include <cmath>
#include <thread>

using namespace er;

class TestAudio : public QObject {
    Q_OBJECT
    static QString clip(const char* n) { return QStringLiteral(ER_TEST_MEDIA "/") + QLatin1String(n); }

    MediaStore media;
    MediaAsset tone, video;
    std::optional<Waveform> toneWave;

    AudioSources sources() {
        AudioSources s;
        s.sound = [this](const Uuid& id) -> std::optional<std::pair<QString, std::pair<double, double>>> {
            auto p = media.probe(id);
            if (!p || !p->hasAudio) return std::nullopt;
            return std::make_pair(media.soundPath(id), std::make_pair(0.0, p->audioEnd));
        };
        s.reversed = [this](const Uuid& id) -> std::optional<std::pair<QString, double>> {
            return ReverseAudio::render(media.soundPath(id));
        };
        s.peaks = [this](const Uuid&, double* pps) -> const std::vector<float>* {
            *pps = toneWave->peaksPerSecond;
            return &toneWave->peaks;
        };
        return s;
    }

    static double rms(const std::vector<float>& s, size_t from = 0, size_t to = 0) {
        if (!to) to = s.size();
        double a = 0;
        for (size_t i = from; i < to; ++i) a += double(s[i]) * s[i];
        return std::sqrt(a / std::max<size_t>(1, to - from));
    }

    /// The strongest frequency, by counting zero crossings of the left channel.
    static double frequency(const std::vector<float>& s, int rate) {
        int crossings = 0;
        for (size_t i = 2; i < s.size(); i += 2) if ((s[i - 2] < 0) != (s[i] < 0)) ++crossings;
        return crossings / 2.0 / (double(s.size() / 2) / rate);
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        if (!QFile::exists(clip("b_tone.wav"))) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        tone = *media.importFile(clip("b_tone.wav"));
        video = *media.importFile(clip("a_main.mp4"));
        toneWave = WaveformReader::read(clip("b_tone.wav"));
        QVERIFY(toneWave && toneWave->peaks.size() == 800);
    }

    // Gain check (SelfTest): a lone tone at 0 dB, +6 dB and −12 dB.
    void gains() {
        double base = 0;
        for (double db : {0.0, 6.0, -12.0}) {
            Project p;
            p.assets = {tone};
            Clip c(p.audioTracks[0].id, ClipKind::audio, 0, 2); c.assetID = tone.id; c.gainDB = db;
            p.clips = {c};
            Mixer m;
            m.setPlan(AudioPlanBuilder::build(p, sources()));
            const auto s = m.renderRange(0.5, 1.5);
            const double r = rms(s);
            if (db == 0) base = r;
            QVERIFY2(std::abs(20 * std::log10(r / base) - db) < 0.01, qPrintable(QString::number(db)));
        }
        QVERIFY(std::abs(20 * std::log10(base) + 24.08) < 0.05); // the tone itself, unchanged
    }

    void fades() {
        Project p;
        p.assets = {tone};
        Clip c(p.audioTracks[0].id, ClipKind::audio, 0, 2); c.assetID = tone.id; c.fadeIn = 1; c.fadeInCurve = FadeCurve::smooth;
        p.clips = {c};
        const AudioPlan plan = AudioPlanBuilder::build(p, sources());
        // 16 linear pieces approximating the S-curve, as on the Mac.
        QCOMPARE(plan.lanes[0].volume.size(), size_t(16));
        QVERIFY(std::abs(plan.lanes[0].volume.at(0.5) - 0.5) < 1e-6);
        QVERIFY(std::abs(plan.lanes[0].volume.at(0.25) - applyFadeCurve(FadeCurve::smooth, 0.25)) < 0.01);
        QCOMPARE(plan.lanes[0].volume.at(1.5), 1.0);
        Mixer m;
        m.setPlan(plan);
        const auto s = m.renderRange(0, 2);
        QVERIFY(rms(s, 0, 4800) < rms(s, 96000 - 4800, 96000) * 0.05);
    }

    void ducking() {
        Project p;
        p.assets = {tone, video};
        Clip music(p.audioTracks[1].id, ClipKind::audio, 0, 4); music.assetID = tone.id;
        Clip voice(p.audioTracks[0].id, ClipKind::audio, 1, 2); voice.assetID = video.id;
        p.clips = {music, voice};
        p.audioTracks[1].duckUnder = p.audioTracks[0].id;
        const AudioPlan plan = AudioPlanBuilder::build(p, sources());
        const AudioLane* lane = nullptr;
        for (const auto& l : plan.lanes) if (l.track == p.audioTracks[1].id) lane = &l;
        QVERIFY(lane);
        // 50 ms pieces over the music clip, down to −12 dB while the voice talks.
        QCOMPARE(lane->volume.size(), size_t(80));
        QVERIFY(std::abs(lane->volume.at(2.0) - dbToLinear(-12)) < 1e-6);
        QCOMPARE(lane->volume.at(0.5), 1.0);
    }

    // Twice as fast: half as long, the same level, the same pitch (the Mac's `.spectral`).
    void speed() {
        Project p;
        p.assets = {tone};
        Clip c(p.audioTracks[0].id, ClipKind::audio, 0, 2, 0, 2); c.assetID = tone.id;
        p.clips = {c};
        const AudioPlan plan = AudioPlanBuilder::build(p, sources());
        QCOMPARE(plan.lanes[0].voices[0].pieces.size(), size_t(1));
        QVERIFY(std::abs(plan.lanes[0].voices[0].pieces[0].rate() - 2) < 1e-9);
        Mixer m;
        m.setPlan(plan);
        const auto s = m.renderRange(0.3, 1.7);
        const double f = frequency(s, 48000);
        QVERIFY2(std::abs(f - 440) < 5, qPrintable(QString::number(f)));
        QVERIFY2(std::abs(20 * std::log10(rms(s)) + 24.08) < 1.0, qPrintable(QString::number(20 * std::log10(rms(s)))));
        // Too fast for sound (over 32×): silent, as on the Mac.
        p.clips[0].speed = 40;
        QVERIFY(AudioPlanBuilder::build(p, sources()).lanes.empty());
    }

    void ramp() {
        Project p;
        p.assets = {tone};
        Clip c(p.audioTracks[0].id, ClipKind::audio, 0, 4); c.assetID = tone.id;
        p.clips = {c};
        p.setRamp({c.id}, 2, 0, 0.25, 0.75, 1, 1, 1);
        const AudioPlan plan = AudioPlanBuilder::build(p, sources());
        // Short pieces through each ramp (max(4, min(90, ceil(span × 30))) steps).
        QVERIFY(plan.lanes[0].voices[0].pieces.size() > 8);
        Mixer m;
        m.setPlan(plan);
        const auto s = m.renderRange(0, p.duration());
        QVERIFY(std::abs(frequency(s, 48000) - 440) < 10);
    }

    void reverse() {
        QString err;
        auto r = ReverseAudio::render(clip("b_tone.wav"), &err);
        QVERIFY2(r, qPrintable(err));
        QVERIFY(std::abs(r->second - 4.0) < 0.01);
        Project p;
        p.assets = {tone};
        Clip c(p.audioTracks[0].id, ClipKind::audio, 0, 2); c.assetID = tone.id; c.reversed = true;
        p.clips = {c};
        const AudioPlan plan = AudioPlanBuilder::build(p, sources());
        QVERIFY(plan.lanes[0].voices[0].reversed);
        Mixer m;
        m.setPlan(plan);
        QVERIFY(std::abs(20 * std::log10(rms(m.renderRange(0.2, 1.8))) + 24.08) < 0.1);
    }

    // EBU Tech 3341 case 1: a 1 kHz stereo sine at −23 dBFS reads −23.0 LUFS (±0.1).
    void loudness() {
        std::vector<float> s(48000 * 20 * 2);
        const double a = std::pow(10, -23 / 20.0);
        for (size_t i = 0; i < s.size() / 2; ++i) s[i * 2] = s[i * 2 + 1] = float(a * std::sin(2 * M_PI * 1000 * i / 48000.0));
        auto r = LoudnessMeter::measure(s);
        QVERIFY(r);
        QVERIFY2(std::abs(r->lufs + 23) < 0.1, qPrintable(QString::number(r->lufs)));
        QVERIFY(std::abs(r->peakDB + 23) < 0.01);
        QVERIFY(std::abs(LoudnessMeter::targetGain(*r, -14) - 9) < 0.1);
        // Silence is below the absolute gate.
        QVERIFY(!LoudnessMeter::measure(std::vector<float>(48000 * 4 * 2)));
    }

    void waveformCache() {
        const QString path = WaveformReader::cachePath(clip("b_tone.wav"));
        QVERIFY(path.endsWith(QLatin1String(".wave")) && path.contains(QLatin1String("b_tone-")));
        QVERIFY(WaveformReader::save(*toneWave, path));
        auto back = WaveformReader::load(path);
        QVERIFY(back && back->peaks == toneWave->peaks && back->rms == toneWave->rms);
    }

    void streamClock() {
        Project p;
        p.assets = {tone};
        Clip c(p.audioTracks[0].id, ClipKind::audio, 0, 3); c.assetID = tone.id;
        p.clips = {c};
        AudioStream stream;
        stream.setPlan(AudioPlanBuilder::build(p, sources(), true));
        stream.play(1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const double t = stream.currentTime();
        QVERIFY2(t > 1.15 && t < 1.6, qPrintable(QString::number(t)));
        stream.setLoop(std::make_pair(1.0, 1.5));
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        QVERIFY(stream.currentTime() >= 1.0 && stream.currentTime() <= 1.5 + 1e-6);
        QVERIFY(stream.takeWrap());
        stream.pause();
        QVERIFY(!stream.isPlaying());
    }

    void outputDelay() {
        // Bluetooth headphones take the sound 0.15–0.3 s to play (as the sound server reports it; forced here): what's
        // heard, which the picture shows, is that much behind what's handed over, and never before where play started.
        qputenv("EDITROOM_SINK_LATENCY", "0.25");
        Project p;
        p.assets = {tone};
        Clip c(p.audioTracks[0].id, ClipKind::audio, 0, 2); c.assetID = tone.id;
        p.clips = {c};
        {
            AudioStream stream;
            stream.setPlan(AudioPlanBuilder::build(p, sources(), true));
            stream.play(0.5);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            // Started: the picture waits at the start with the sound, rather than running ahead of it.
            QCOMPARE(stream.currentTime(), 0.5);
            std::this_thread::sleep_for(std::chrono::milliseconds(450));
            QVERIFY(std::abs(stream.sinkLatency() - 0.25) < 1e-9);
            const double t = stream.currentTime();
            // 0.6 s played, 0.25 s of it still on its way: about 0.85 heard.
            QVERIFY2(t > 0.75 && t < 0.95, qPrintable(QString::number(t)));
            // To the end: what's still on its way plays out, so the end is reached.
            std::this_thread::sleep_for(std::chrono::milliseconds(1800));
            QVERIFY(stream.ended());
            QVERIFY2(stream.currentTime() >= 2.0 - 1e-6, qPrintable(QString::number(stream.currentTime())));
        }
        qunsetenv("EDITROOM_SINK_LATENCY");
    }
};

QTEST_MAIN(TestAudio)
#include "tst_audio.moc"
