// `.room` round-trip: every file in tests/format/corpus opens, saves and re-opens unchanged (semantically
// equal JSON), and files written in the Mac's exact style come back byte for byte.
//
// The corpus has only synthetic files so far, written by hand in JSONEncoder's style from the Swift model.
// Mac-saved projects go in here at the first sync with a Mac (see docs/PARITY.md, "File format").
#include "model/Codec.h"
#include "model/Json.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace er;

class TestFormat : public QObject {
    Q_OBJECT

    static QByteArray read(const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    static QString corpus() { return QStringLiteral(ER_TEST_DATA "/format/corpus"); }

private slots:
    void roundtrip_data() {
        QTest::addColumn<QString>("path");
        const auto files = QDir(corpus()).entryList({QStringLiteral("*.room")}, QDir::Files, QDir::Name);
        QVERIFY(!files.isEmpty());
        for (const auto& f : files) QTest::newRow(qPrintable(f)) << corpus() + u'/' + f;
    }
    void roundtrip() {
        QFETCH(QString, path);
        const QByteArray original = read(path);
        auto res = codec::decodeDocument(original);
        QVERIFY2(res.document, qPrintable(res.error));
        const QByteArray saved = codec::saveBytes(*res.document);
        auto again = codec::decodeDocument(saved);
        QVERIFY(again.document);
        QVERIFY(*again.document == *res.document);
        QVERIFY(json::semanticallyEqual(QJsonDocument::fromJson(original).object(), QJsonDocument::fromJson(saved).object()));
        if (!original.endsWith('\n')) QCOMPARE(QString::fromUtf8(saved), QString::fromUtf8(original));
    }

    void keepsWhatLxDoesntUse() {
        auto res = codec::decodeDocument(read(corpus() + QStringLiteral("/synthetic-all-fields.room")));
        QVERIFY(res.document);
        const Project& p = res.document->project;
        QCOMPARE(p.assets[0].bookmark->constData(), "bookmarkdata");
        QVERIFY(p.assets[0].playablePath);
        QVERIFY(p.clips[0].extra.contains(QStringLiteral("unknownClipKey")));
        QVERIFY(res.document->extra.contains(QStringLiteral("futureDocumentKey")));
        QCOMPARE(p.captionFixes->size(), size_t(1));
        QCOMPARE(p.captionFixes->begin()->second[1].merged, std::optional<bool>(true));
        QCOMPARE(p.clips[0].transform->zoom->mode, ZoomMove::Mode::inOut);
        QCOMPARE(p.clips[1].text.string, QStringLiteral("The “end” — or is it?\nYes."));
        QCOMPARE(p.magicHistory->front().date, 786000000.5);
    }

    // A new project written by LX: what the Mac's `Project()` would write (every non-optional key, no
    // optional ones), and it reads back equal.
    void freshProject() {
        SavedDocument d;
        const QJsonObject o = codec::encode(d);
        QCOMPARE(o.value(QStringLiteral("version")).toInt(), 1);
        const QJsonObject p = o.value(QStringLiteral("project")).toObject();
        QStringList keys = p.keys();
        std::sort(keys.begin(), keys.end());
        QCOMPARE(keys, (QStringList{"assets", "audioTracks", "clips", "fps", "height", "settingsLocked", "videoTracks", "width"}));
        const QJsonObject track = p.value(QStringLiteral("videoTracks")).toArray().first().toObject();
        QVERIFY(!track.contains(QStringLiteral("duckUnder")));
        QCOMPARE(track.value(QStringLiteral("id")).toString(), track.value(QStringLiteral("id")).toString().toUpper());
        auto back = codec::decodeDocument(codec::saveBytes(d));
        QVERIFY(back.document && *back.document == d);
    }

    void rejectsWhatTheMacRejects() {
        auto bad = codec::decodeDocument(R"({"version":1,"project":{"clips":[{"kind":"hologram"}]}})");
        QVERIFY(!bad.document);
        QVERIFY2(bad.error.contains(QStringLiteral("project.clips[0].kind")), qPrintable(bad.error));
        QVERIFY(!codec::decodeDocument("not json").document);
    }
};

QTEST_GUILESS_MAIN(TestFormat)
#include "tst_format.moc"
