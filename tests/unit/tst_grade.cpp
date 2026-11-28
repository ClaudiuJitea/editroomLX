// Grade maths: the FILM PRINT check from SelfTest.swift, recipes, wheels, .cube files and the baker.
#include "model/ColorGrade.h"
#include "model/Codec.h"
#include <QJsonDocument>
#include <QTest>
#include <cmath>

using namespace er;

class TestGrade : public QObject {
    Q_OBJECT
private slots:
    // Film print: the app's Look gives the same colours as the mock it was approved from.
    void filmPrint() {
        const std::vector<std::pair<RGB, RGB>> cases = {
            {{0.80, 0.55, 0.42}, {0.8603, 0.6593, 0.5182}},
            {{0.70, 0.10, 0.12}, {0.6969, 0.1364, 0.1386}},
            {{0.20, 0.50, 0.25}, {0.3269, 0.5724, 0.3360}},
            {{0.50, 0.50, 0.50}, {0.6021, 0.5987, 0.5938}},
            {{0.10, 0.12, 0.20}, {0.1167, 0.1203, 0.1584}},
            {{0.95, 0.93, 0.90}, {0.9350, 0.9800, 0.9546}},
        };
        double worst = 0;
        for (const auto& [in, want] : cases) {
            const RGB got = LookMath::filmPrint(in);
            worst = std::max({worst, std::abs(got.r - want.r), std::abs(got.g - want.g), std::abs(got.b - want.b)});
        }
        QVERIFY2(worst < 0.002, qPrintable(QString::number(worst)));
        QVERIFY(BuiltInLook::named(QStringLiteral("Film print")));
        QVERIFY(BuiltInLook::named(QStringLiteral("Faded"))); // retired, still read
        QCOMPARE(BuiltInLook::all().size(), size_t(8));
        QCOMPARE(BuiltInLook::all().front().first, QStringLiteral("Warm film"));
    }

    void neutral() {
        ColorGrade g;
        QVERIFY(g.isNeutral());
        g.lookIntensity = 0.4; // without a look its strength doesn't count
        QVERIFY(g.isNeutral());
        QVERIFY(!GradeBaker::cube(g));
        g.look = QStringLiteral("Mono");
        QVERIFY(!g.isNeutral());
        QCOMPARE(g.lookLabel(), QStringLiteral("Mono"));
        g.look = QStringLiteral("/home/me/LUTs/My Look.cube");
        QCOMPARE(g.lookLabel(), QStringLiteral("My Look"));
    }

    void wheels() {
        WheelPoint p{1, 0, {}};
        const RGB o = p.offset();
        QVERIFY(std::abs(o.r - 0.22) < 1e-12 && std::abs(o.g - 0.22 * std::cos(-2 * M_PI / 3)) < 1e-12);
        QCOMPARE(WheelPoint().offset(), (RGB{0, 0, 0}));
    }

    void recipe() {
        GradeRecipe r;
        QCOMPARE(r.apply({0.25, 0.5, 0.75}), (RGB{0.25, 0.5, 0.75}));
        r.exposure = 1;
        const RGB e = r.apply({0.25, 0.25, 0.25});
        QVERIFY(std::abs(e.r - 0.5) < 1e-12);
        GradeRecipe mono; mono.saturation = -100;
        const RGB m = mono.apply({1, 0, 0});
        QVERIFY(std::abs(m.r - 0.2126) < 1e-12 && std::abs(m.g - 0.2126) < 1e-12);
    }

    void cubeFile() {
        // Identity 2³ cube, red fastest; trilinear lookups give the input back.
        const QString text = QStringLiteral("# test\nTITLE \"id\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
        auto lut = CubeLUT::parse(text);
        QVERIFY(lut);
        const RGB s = lut->sample({0.3, 0.6, 0.9});
        QVERIFY(std::abs(s.r - 0.3) < 1e-6 && std::abs(s.g - 0.6) < 1e-6 && std::abs(s.b - 0.9) < 1e-6);
        QVERIFY(!CubeLUT::parse(QStringLiteral("LUT_3D_SIZE 2\n0 0 0\n")));
    }

    void baker() {
        ColorGrade g;
        g.exposure = 0.5;
        auto cube = GradeBaker::cube(g);
        QVERIFY(cube);
        const int n = GradeBaker::dimension;
        QCOMPARE(cube->size(), size_t(n * n * n * 4));
        // Alpha stays 1; values are clamped to 0…1; black stays black under exposure.
        QCOMPARE((*cube)[3], 1.0f);
        QCOMPARE((*cube)[0], 0.0f);
        const size_t white = (size_t(n * n * n) - 1) * 4;
        QCOMPARE((*cube)[white], 1.0f);
        // Cached: the same grade gives the same data.
        QCOMPARE(GradeBaker::cube(g).get(), cube.get());
        // A look at half strength sits between none and full.
        ColorGrade mono; mono.look = QStringLiteral("Mono"); mono.lookIntensity = 0.5;
        auto half = GradeBaker::cube(mono);
        const size_t red = size_t(n - 1) * 4; // r = 1, g = b = 0
        QVERIFY((*half)[red] < 1.0f && (*half)[red + 1] > 0.0f);
    }

    void levels() {
        // The Mac's GRADE LEVELS self-test: a grade from before has none (and still opens), lifting the shadows
        // brightens the darks more than the brights, pulling the highlights down darkens the brights more, it's kept,
        // and an off grade draws nothing, is kept, and comes back.
        const QByteArray old = R"({"look":"Mono","lookIntensity":1,"exposure":0.5,"contrast":0,"saturation":0,"temperature":0,"tint":0})";
        const ColorGrade before = codec::decodeGrade(QJsonDocument::fromJson(old).object());
        QCOMPARE(before.shadowsLevel, 0.0);
        QVERIFY(!before.off);
        QCOMPARE(ColorGrade::levelOffset(100), 0.2);
        QCOMPARE(ColorGrade::levelOffset(-250), -0.2);
        const int n = GradeBaker::dimension;
        auto grey = [n](const std::vector<float>& c, int i) { return c[((size_t(i) * n + i) * n + i) * 4]; };
        ColorGrade lift; lift.shadowsLevel = 60;
        auto lc = GradeBaker::cube(lift);
        QVERIFY(lc && !lift.isNeutral());
        const int dark = n / 6, bright = n - 1 - n / 6;
        const double darkUp = grey(*lc, dark) - double(dark) / (n - 1), brightUp = grey(*lc, bright) - double(bright) / (n - 1);
        QVERIFY2(darkUp > 0.01 && darkUp > brightUp * 2, qPrintable(QStringLiteral("%1 %2").arg(darkUp).arg(brightUp)));
        ColorGrade pull; pull.highlightsLevel = -60;
        auto pc = GradeBaker::cube(pull);
        const double darkDown = double(dark) / (n - 1) - grey(*pc, dark), brightDown = double(bright) / (n - 1) - grey(*pc, bright);
        QVERIFY2(brightDown > 0.01 && brightDown > darkDown * 2, qPrintable(QStringLiteral("%1 %2").arg(darkDown).arg(brightDown)));
        // Kept through a save.
        const ColorGrade kept = codec::decodeGrade(codec::encode(lift));
        QCOMPARE(kept.shadowsLevel, 60.0);
        // Off: nothing drawn, still not neutral (the grade is kept), and back on gives the same cube.
        ColorGrade off = lift; off.off = true;
        QVERIFY(!GradeBaker::cube(off));
        QVERIFY(!off.isNeutral());
        QVERIFY(codec::decodeGrade(codec::encode(off)).off);
        off.off = false;
        QCOMPARE(GradeBaker::cube(off).get(), lc.get());
    }
};

QTEST_GUILESS_MAIN(TestGrade)
#include "tst_grade.moc"
