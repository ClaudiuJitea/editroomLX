// Transcripts: whisper.cpp hears what's said (whisper.cpp's own JFK sample), word by word with times; the Mac's
// search helpers (says, snippet, occurrences, sentences); the queue, the saved copy, and no model meaning none.
// Needs the speech model: EDITROOM_WHISPER_MODEL=<ggml-base.bin> (skipped without it).
#include "engine/Transcripts.h"
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace er;

class TestTranscripts : public QObject {
    Q_OBJECT
    const QString sample = QStringLiteral(ER_SOURCE "/third_party/whisper.cpp/samples/jfk.wav");
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
    }

    void speechLanguage() {
        // The Mac's SPEECH LANGUAGE check: one the model can't hear is said (by name), not guessed at; one it can is
        // used; Automatic is heard from the speech. The setting is put back as it was.
        QSettings settings;
        const QVariant before = settings.value(QLatin1String(Transcripts::languageKey));
        settings.setValue(QLatin1String(Transcripts::languageKey), QStringLiteral("kl_GL"));
        const auto kl = Transcripts::resolve();
        settings.setValue(QLatin1String(Transcripts::languageKey), QStringLiteral("es_ES"));
        const auto es = Transcripts::resolve();
        settings.setValue(QLatin1String(Transcripts::languageKey), QString());
        const auto automatic = Transcripts::resolve();
        if (before.isValid()) settings.setValue(QLatin1String(Transcripts::languageKey), before);
        else settings.remove(QLatin1String(Transcripts::languageKey));
        QVERIFY2(!kl.code && !kl.name.isEmpty() && kl.name != QLatin1String("kl_GL"), qPrintable(kl.name));
        QCOMPARE(es.code.value_or(QString()), QStringLiteral("es"));
        QCOMPARE(automatic.code.value_or(QString()), QStringLiteral("auto"));
        // Settings lists the model's languages by name, Spanish among them.
        const auto all = Transcripts::supportedLanguages();
        QVERIFY(all.size() > 50);
        QVERIFY(std::any_of(all.begin(), all.end(), [](const auto& l) { return l.first == QLatin1String("es") && l.second == QLatin1String("Spanish"); }));
    }

    void hears() {
        if (!Transcripts::available()) QSKIP("No speech model: set EDITROOM_WHISPER_MODEL");
        auto t = Transcripts::transcribe(sample);
        QVERIFY(t);
        QVERIFY2(t->says(QStringLiteral("ask not what your country can do for you")), qPrintable(t->text()));
        QCOMPARE(t->language, QStringLiteral("en"));
        // Word by word, in order, with times inside the 11 s clip.
        QVERIFY(t->words.size() > 15);
        for (size_t i = 1; i < t->words.size(); ++i) QVERIFY(t->words[i].start >= t->words[i - 1].start - 0.01);
        QVERIFY(t->words.back().end < 11.5);
        auto s = t->snippet(QStringLiteral("your coun"), true);
        QVERIFY(s && s->match.toLower().startsWith(QStringLiteral("your")));
        QVERIFY(!t->occurrences(QStringLiteral("country")).empty());
        QVERIFY(!t->sentences().empty());
        QVERIFY(!t->speech().empty());
    }

    void queueAndCache() {
        if (!Transcripts::available()) QSKIP("No speech model: set EDITROOM_WHISPER_MODEL");
        Transcripts store;
        std::optional<Transcript> got;
        bool called = false;
        store.ready(sample, [&](std::optional<Transcript> t) { got = t; called = true; });
        store.waitForDone();
        for (int i = 0; i < 50 && !called; ++i) QTest::qWait(20);
        QVERIFY(called && got && !got->isEmpty());
        QCOMPARE(store.state(sample), Transcripts::State::ready);
        // Kept on disk: another store has it at once.
        Transcripts again;
        auto t = again.transcript(sample);
        QVERIFY(t);
        QCOMPARE(t->words.size(), got->words.size());
    }

    void noModel() {
        qputenv("EDITROOM_WHISPER_MODEL", "/nonexistent/model.bin");
        QVERIFY(!Transcripts::available());
        QVERIFY(!Transcripts::transcribe(sample));
    }
};

QTEST_MAIN(TestTranscripts)
#include "tst_transcripts.moc"
