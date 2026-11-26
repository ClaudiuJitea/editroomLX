// Check for Updates: version numbers compared ("1.2.10" is newer than "1.2.9"; "v" and pre-release tails left out),
// GitHub's releases/latest answer read (newer, the same, none published yet, an error).
#include "ui/Updates.h"
#include <QTest>

using namespace er;

class TestUpdates : public QObject {
    Q_OBJECT
private slots:
    void versions() {
        QVERIFY(Updates::compare(QStringLiteral("1.2.10"), QStringLiteral("1.2.9")) > 0);
        QVERIFY(Updates::compare(QStringLiteral("v1.0.3"), QStringLiteral("1.0.3")) == 0);
        QVERIFY(Updates::compare(QStringLiteral("1.0"), QStringLiteral("1.0.1")) < 0);
        QVERIFY(Updates::compare(QStringLiteral("2.0.0-beta.1"), QStringLiteral("1.9.9")) > 0);
    }
    void reading() {
        const QByteArray newer = R"({"tag_name":"v1.1.0","name":"Editroom LX 1.1","body":"Faster export.","html_url":"https://github.com/ClaudiuJitea/editroomLX/releases/tag/v1.1.0"})";
        auto r = Updates::read(newer, 200, QStringLiteral("1.0.3"));
        QVERIFY(r.ok && r.newer);
        QCOMPARE(r.version, QStringLiteral("1.1.0"));
        QCOMPARE(r.notes, QStringLiteral("Faster export."));
        QVERIFY(r.url.endsWith(QStringLiteral("v1.1.0")));
        r = Updates::read(R"({"tag_name":"1.0.3"})", 200, QStringLiteral("1.0.3"));
        QVERIFY(r.ok && !r.newer);
        // No release published yet.
        r = Updates::read(R"({"message":"Not Found"})", 404, QStringLiteral("1.0.3"));
        QVERIFY(r.ok && !r.newer && r.none);
        // Rate-limited or broken: an error, said.
        r = Updates::read(R"({"message":"API rate limit exceeded"})", 403, QStringLiteral("1.0.3"));
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QStringLiteral("API rate limit exceeded"));
    }
};

QTEST_MAIN(TestUpdates)
#include "tst_updates.moc"
