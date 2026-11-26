// Scene tags: the Mac's searching (shows, typing, aliases, stems, matching) on tags, the JSON kept on disk, and,
// with the model (EDITROOM_CLIP_MODEL=<folder>, skipped without it), CLIP seeing a real photo and a video.
#include "engine/Faces.h"
#include "engine/SceneTagStore.h"
#include <QFile>
#include <QStandardPaths>
#include <QTest>

using namespace er;

class TestScenes : public QObject {
    Q_OBJECT
    const QString mug = QStringLiteral("/usr/share/backgrounds/ezspain-Ubuntu_Coffee_Mug_Light.png");
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void searching() {
        SceneTags t{{{"swimming pool", 0.8, {1.0}}, {"portal", 0.5, {2.0}}, {"laughter", 0.4, {3.0}}, {"dog", 0.9, {}}}};
        QVERIFY(t.shows("pool"));
        QVERIFY(t.shows("Pools"));
        QVERIFY(t.shows("door"));          // portal's everyday word
        QVERIFY(t.shows("laughing"));      // same stem
        QVERIFY(!t.shows("po"));
        QVERIFY(t.shows("po", true));      // while it's typed
        QVERIFY(t.shows("puppy"));
        QVERIFY(!t.shows("cat"));
        QCOMPARE(t.matching("pool").size(), size_t(1));
        QCOMPARE(t.top(2), (QStringList{"dog", "swimming pool"}));
        t.add({{4.0, {"car", "dog"}}});
        QCOMPARE(t.tags.back().name, QStringLiteral("car"));
        QCOMPARE(t.tags.back().confidence, 0.6);
        QCOMPARE(SceneTags::fromJson(t.json()), std::optional<SceneTags>(t));
    }

    void seesAPhoto() {
        if (!SceneTagStore::available()) QSKIP("No scene model: set EDITROOM_CLIP_MODEL");
        if (!QFile::exists(mug)) QSKIP("No sample photo");
        const auto all = SceneTagStore::classify(QImage(mug));
        QVERIFY(!all.empty());
        QStringList top;
        for (int i = 0; i < 8; ++i) top << QStringLiteral("%1 %2").arg(all[i].first).arg(all[i].second, 0, 'f', 3);
        qInfo().noquote() << top.join(", ");
        auto t = SceneTagStore::seePhoto(mug);
        QVERIFY(t && !t->tags.empty());
        QVERIFY2(t->shows("coffee") || t->shows("cup") || t->shows("mug"), qPrintable(t->top(8).join(", ")));
    }

    void queueAndCache() {
        if (!SceneTagStore::available()) QSKIP("No scene model: set EDITROOM_CLIP_MODEL");
        if (!QFile::exists(mug)) QSKIP("No sample photo");
        SceneTagStore store;
        store.forget(mug);
        store.make(mug, false, 0);
        store.waitForDone();
        QCoreApplication::processEvents();
        auto t = store.tags(mug);
        QVERIFY(t && t->shows("cup"));
        SceneTagStore again;
        QCOMPARE(again.tags(mug), t);
    }

    /// Faces (YuNet, which comes with Editroom): none in a mug; some in a photo of people (ER_FACE_SAMPLE).
    void faces() {
        if (!Faces::available()) QSKIP("No ONNX Runtime");
        if (QFile::exists(mug)) QCOMPARE(Faces::count(QImage(mug).scaledToWidth(320)), 0);
        const QString people = qEnvironmentVariable("ER_FACE_SAMPLE");
        if (people.isEmpty()) QSKIP("Set ER_FACE_SAMPLE to a photo with faces");
        const auto f = Faces::find(QImage(people));
        qInfo() << "faces" << f.size();
        QVERIFY(Faces::count(QImage(people)) >= 1);
    }

    /// The aesthetics: a sharp picture scores above the same picture blurred to mush.
    void aesthetics() {
        if (!SceneTagStore::available()) QSKIP("No scene model: set EDITROOM_CLIP_MODEL");
        if (!QFile::exists(mug)) QSKIP("No sample photo");
        const QImage sharp = QImage(mug).scaledToWidth(640, Qt::SmoothTransformation);
        const QImage mush = sharp.scaledToWidth(16, Qt::SmoothTransformation).scaledToWidth(640, Qt::FastTransformation);
        for (const QString& f : qEnvironmentVariable("ER_AESTHETICS_SAMPLES").split(u':', Qt::SkipEmptyParts)) {
            auto x = SceneTagStore::aesthetics(QImage(f).scaledToWidth(320, Qt::SmoothTransformation));
            qInfo().noquote() << f.section(u'/', -1) << x->score << x->utility;
        }
        auto a = SceneTagStore::aesthetics(sharp), b = SceneTagStore::aesthetics(mush);
        QVERIFY(a && b);
        qInfo() << "sharp" << a->score << a->utility << "mush" << b->score;
        QVERIFY(a->score > b->score);
        // A photo of people is a photo; Settings' own screenshot (ER_SHOTS from tst_settings) is a screenshot.
        if (const QString p = qEnvironmentVariable("ER_FACE_SAMPLE"); !p.isEmpty()) QVERIFY(!SceneTagStore::aesthetics(QImage(p))->utility);
    }
};

QTEST_MAIN(TestScenes)
#include "tst_scenes.moc"
