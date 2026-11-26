// The Swift-JSONEncoder-compatible writer.
#include "model/Json.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace er;

class TestJson : public QObject {
    Q_OBJECT
private slots:
    void doubles_data() {
        QTest::addColumn<double>("value");
        QTest::addColumn<QString>("swift");
        // Swift `Double.description`, then JSONEncoder drops a trailing ".0".
        QTest::newRow("int") << 1920.0 << "1920";
        QTest::newRow("zero") << 0.0 << "0";
        QTest::newRow("negzero") << -0.0 << "-0";
        QTest::newRow("tenth") << 0.1 << "0.1";
        QTest::newRow("third") << 1.0 / 3 << "0.3333333333333333";
        QTest::newRow("fps") << 29.97002997002997 << "29.97002997002997";
        QTest::newRow("small-decimal") << 0.0001 << "0.0001";
        QTest::newRow("exp-small") << 0.00001 << "1e-05";
        QTest::newRow("exp-small2") << 1.5e-7 << "1.5e-07";
        QTest::newRow("neg") << -12.0 << "-12";
        QTest::newRow("big-decimal") << 1e16 << "10000000000000000";
        QTest::newRow("exp-big") << 1e20 << "1e+20";
        QTest::newRow("sum") << 0.1 + 0.2 << "0.30000000000000004";
        QTest::newRow("frame") << 1.0 / 60 << "0.016666666666666666";
    }
    void doubles() {
        QFETCH(double, value);
        QFETCH(QString, swift);
        QCOMPARE(json::formatNumber(value), swift);
    }

    void strings() {
        QCOMPARE(json::quote(QStringLiteral("/Users/me/a \"b\".mov")), QStringLiteral("\"\\/Users\\/me\\/a \\\"b\\\".mov\""));
        QCOMPARE(json::quote(QStringLiteral("line\nnext\ttab")), QStringLiteral("\"line\\nnext\\ttab\""));
        QCOMPARE(json::quote(QStringLiteral("−∞ dB “x”")), QStringLiteral("\"−∞ dB “x”\""));
        QCOMPARE(json::quote(QString(QChar(1))), QStringLiteral("\"\\u0001\""));
    }

    void pretty() {
        QJsonObject o{{"b", 1}, {"a", QJsonArray{1.5, true}}, {"empty", QJsonArray{}}, {"o", QJsonObject{}}};
        const QByteArray want = "{\n  \"a\" : [\n    1.5,\n    true\n  ],\n  \"b\" : 1,\n  \"empty\" : [\n\n  ],\n  \"o\" : {\n\n  }\n}";
        QCOMPARE(json::write(o, true), want);
        QCOMPARE(json::write(o, false), QByteArray("{\"a\":[1.5,true],\"b\":1,\"empty\":[],\"o\":{}}"));
    }

    void sortedKeys() {
        // Foundation sorts keys case-insensitively and numerically, then by code point.
        QStringList keys{"b", "B", "a10", "a2", "A1", "trackID", "transform", "transitionIn", "text"};
        std::sort(keys.begin(), keys.end(), json::keyLess);
        QCOMPARE(keys, (QStringList{"A1", "a2", "a10", "B", "b", "text", "trackID", "transform", "transitionIn"}));
    }

    void uuids() {
        const QUuid u = QUuid::fromString(QStringLiteral("6f1d8c64-1b2e-4c38-9a6e-0b9c1f6a1b2c"));
        QCOMPARE(json::uuidString(u), QStringLiteral("6F1D8C64-1B2E-4C38-9A6E-0B9C1F6A1B2C"));
        QVERIFY(json::parseUuid(QStringLiteral("6f1d8c64-1b2e-4c38-9a6e-0b9c1f6a1b2c")));
        QVERIFY(!json::parseUuid(QStringLiteral("{6f1d8c64-1b2e-4c38-9a6e-0b9c1f6a1b2c}")));
        QVERIFY(!json::parseUuid(QStringLiteral("nope")));
    }

    void semantic() {
        QJsonObject a{{"captionFixes", QJsonArray{"A", QJsonArray{1}, "B", QJsonArray{2}}}, {"x", 1}};
        QJsonObject b{{"x", 1.0}, {"captionFixes", QJsonArray{"B", QJsonArray{2}, "A", QJsonArray{1}}}};
        QVERIFY(json::semanticallyEqual(a, b));
        b.insert(QStringLiteral("x"), 2);
        QVERIFY(!json::semanticallyEqual(a, b));
    }
};

QTEST_GUILESS_MAIN(TestJson)
#include "tst_json.moc"
