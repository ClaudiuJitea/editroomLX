// The README's screenshots, staged in the Grey theme: the editor with a cut, the grade and the title inspectors,
// Magic (the box, the thinking scene, the cursors), Settings and Export. Run by scripts/make-readme-shots.sh, which
// makes the demo footage first; skipped without ER_SHOTS and ER_README_MEDIA.
#include "mcp/MCPServer.h"
#include "mcp/MagicAgent.h"
#include "mcp/MagicPlan.h"
#include "mcp/LocalPlanners.h"
#include "mcp/MagicPopover.h"
#include "mcp/MagicShow.h"
#include "mcp/MagicThinking.h"
#include "ui/Controller.h"
#include "ui/ExportDialog.h"
#include "ui/MainWindow.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QDir>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace er;

class TestReadme : public QObject {
    Q_OBJECT
    QString shots, media;
    std::unique_ptr<Controller> c;
    std::unique_ptr<MainWindow> w;
    std::map<QString, MediaAsset> assets;
    Uuid graded, title;

    void settle(int ms = 400) { QTest::qWait(ms); }
    void save(QWidget* widget, const char* name) { widget->grab().save(shots + u'/' + QLatin1String(name) + QStringLiteral(".png")); }

private slots:
    void initTestCase() {
        shots = qEnvironmentVariable("ER_SHOTS");
        media = qEnvironmentVariable("ER_README_MEDIA");
        if (shots.isEmpty() || media.isEmpty()) QSKIP("Run scripts/make-readme-shots.sh");
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        QCoreApplication::setApplicationVersion(QStringLiteral(ER_VERSION));
        Theme::loadFonts();
        Theme::setCurrent(AppTheme::grey);
        Theme::apply();
        c = std::make_unique<Controller>();
        w = std::make_unique<MainWindow>(*c);
        w->resize(1600, 1000);
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w.get()));
        for (const char* f : {"Sunrise.mp4", "Fractal.mp4", "Studio.mp4", "Ocean.mp4", "Soundtrack.wav"}) {
            auto a = c->media().importFile(media + u'/' + QLatin1String(f));
            QVERIFY2(a, f);
            assets[QLatin1String(f)] = *a;
            c->model().changeLibrary([&](Project& p) { p.assets.push_back(*a); });
        }
        // A short cut: four shots on V1 with their sound, a dissolve, a graded shot, a title, music under it.
        c->model().mutate([&](Project& p) {
            p.clips.clear();
            const Uuid v1 = p.videoTracks[0].id, v2 = p.videoTracks[1].id, a1 = p.audioTracks[0].id, a2 = p.audioTracks[1].id;
            double t = 0;
            int k = 0;
            for (const char* f : {"Sunrise.mp4", "Fractal.mp4", "Studio.mp4", "Ocean.mp4"}) {
                const MediaAsset& a = assets[QLatin1String(f)];
                const Uuid link = newUuid();
                Clip v(v1, ClipKind::video, t, 3.0, 1.0);
                v.assetID = a.id;
                v.linkID = link;
                if (k == 1) v.transitionIn = Transition{TransitionKind::dissolve, 0.6, {}};
                if (k == 0) { ColorGrade g; g.look = QStringLiteral("Teal and orange"); g.contrast = 12; g.saturation = 10; v.grade = g; graded = v.id; }
                Clip s(a1, ClipKind::audio, t, 3.0, 1.0);
                s.assetID = a.id;
                s.linkID = link;
                s.gainDB = -14;
                p.clips.push_back(v);
                p.clips.push_back(s);
                t += 3.0;
                ++k;
            }
            Clip text(v2, ClipKind::text, 0.4, 2.6);
            text.text.string = QStringLiteral("Editroom LX");
            text.text.size = 130;
            text.fadeIn = 0.4;
            text.fadeOut = 0.5;
            title = text.id;
            p.clips.push_back(text);
            Clip music(a2, ClipKind::audio, 0, 12);
            music.assetID = assets[QStringLiteral("Soundtrack.wav")].id;
            music.fadeOut = 1.5;
            music.gainDB = -2;
            p.clips.push_back(music);
            Marker m;
            m.time = 6.0;
            m.name = QStringLiteral("Beat drop");
            if (!p.markers) p.markers.emplace();
            p.markers->push_back(m);
        });
        c->model().zoomToFit();
    }

    void editor() {
        c->model().setSelection({graded});
        c->model().seek(1.2);
        settle(2500);
        save(w.get(), "editor");
    }

    void titles() {
        c->model().setSelection({title});
        c->model().seek(1.6);
        settle(1500);
        save(w.get(), "titles");
    }

    void magicBox() {
        MCPServer server(*c);
        MagicAgent agent(*c, server);
        QSettings().setValue(QStringLiteral("magicTab"), QStringLiteral("local"));
        MagicPopover pop(*c, agent, w.get());
        pop.show();
        settle();
        if (auto* box = pop.findChild<QPlainTextEdit*>()) box->setPlainText(QStringLiteral("make it 20 seconds, fast, black and white, and open on @Ocean"));
        settle(300);
        save(&pop, "magic-box");
    }

    void thinking() {
        MagicThinking* t = MagicThinking::begin(*c, QStringLiteral("Local"), QColor::fromRgbF(0.95, 0.95, 0.95));
        QVERIFY(t);
        t->analyze(std::nullopt);
        settle(900);
        auto plan = LocalTitle::plan(QStringLiteral("add a title saying \"The End\" at the end"), 0, c->model().project());
        t->plan(plan.plan->result());
        settle(1500);
        save(w.get(), "magic-thinking");
        t->cancel();
        settle(200);
    }

    void cursors() {
        auto plan = LocalRamp::plan(QStringLiteral("ramp into the cuts"), MagicPointers{1.0, {}, std::nullopt}, c->model().project());
        QVERIFY(plan.plan);
        bool over = false;
        MagicShow::play(*c, *plan.plan, [&](bool) { over = true; });
        settle(350);
        save(w.get(), "magic-cursors");
        QTRY_VERIFY_WITH_TIMEOUT(over, 10000);
        c->model().undo();
    }

    void settings() {
        ShortcutStore keys([] { return QByteArray(); }, [](const std::optional<QByteArray>&) {});
        QSettings().setValue(QStringLiteral("settingsTab"), QStringLiteral("general"));
        {
            SettingsDialog d(*c, keys, w.get());
            d.show();
            settle();
            save(&d, "settings");
        }
        QSettings().setValue(QStringLiteral("settingsTab"), QStringLiteral("updates"));
        {
            SettingsDialog d(*c, keys, w.get());
            d.show();
            settle();
            save(&d, "settings-updates");
        }
        QSettings().setValue(QStringLiteral("settingsTab"), QStringLiteral("magic"));
        SettingsDialog d(*c, keys, w.get());
        d.show();
        settle();
        save(&d, "settings-magic");
    }

    void exportDialog() {
        ExportDialog d(*c, w.get());
        d.show();
        settle();
        save(&d, "export");
    }

    void cleanupTestCase() {
        w.reset();
        c.reset();
    }
};

QTEST_MAIN(TestReadme)
#include "tst_readme.moc"
