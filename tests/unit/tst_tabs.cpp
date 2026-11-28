// Project tabs (1.1): the Workspace (new, open into a blank tab or a new one, already-open projects brought to the
// front, closing, Ctrl+W on the only tab closing its project), background tabs letting go of their player, tabs coming
// back at launch without loading the ones behind, the tab strip, and pasting between projects (the Mac's PASTE ACROSS
// self-test).
#include "model/Codec.h"
#include "ui/MainWindow.h"
#include "ui/TabStrip.h"
#include "ui/Workspace.h"
#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace er;

class TestTabs : public QObject {
    Q_OBJECT
    QTemporaryDir dir;

    QString project(const QString& name, int clips) {
        Project p;
        MediaAsset a;
        a.path = QStringLiteral("/tmp/editroom-tabs/%1.mov").arg(name);
        a.name = name + QStringLiteral(".mov");
        a.kind = MediaKind::video;
        a.duration = 10;
        p.assets = {a};
        for (int i = 0; i < clips; ++i) {
            Clip c(p.videoTracks[0].id, ClipKind::video, i * 2.0, 2);
            c.assetID = a.id;
            p.clips.push_back(c);
        }
        const QString path = dir.filePath(name + QStringLiteral(".room"));
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(codec::saveBytes(SavedDocument{1, p, {}}));
        return path;
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("EditroomTabsTest"));
        QCoreApplication::setApplicationName(QStringLiteral("EditroomTabsTest"));
        QSettings().clear();
        QSettings().setValue(QLatin1String(EditorModel::autosaveKey), true);
    }

    void cleanupTestCase() { QSettings().clear(); }

    void workspace() {
        Workspace w(false);
        EditorModel::openHandler() = [&w](const QString& p) { w.open(p); };
        EditorModel::newTabHandler() = [&w] { w.newTab(); };
        QCOMPARE(w.count(), 1);
        // One empty tab: Close Tab does nothing.
        QVERIFY(!w.canClose());
        const QString a = project(QStringLiteral("Alpha"), 2), b = project(QStringLiteral("Beta"), 1);
        // Opening into the blank tab in front uses it.
        w.open(a);
        QCOMPARE(w.count(), 1);
        QCOMPARE(w.title(0), QStringLiteral("Alpha"));
        QVERIFY(w.canClose());
        // Another project: a tab of its own, in front; the one behind lets go of its player.
        w.open(b);
        QCOMPARE(w.count(), 2);
        QCOMPARE(w.activeIndex(), 1);
        QVERIFY(w.loaded(0)->isBackground());
        QVERIFY(!w.loaded(1)->isBackground());
        QVERIFY(w.loaded(0)->engine().isEmpty());
        // Already open: brought to the front, not opened twice.
        w.open(a);
        QCOMPARE(w.count(), 2);
        QCOMPARE(w.activeIndex(), 0);
        QVERIFY(!w.loaded(0)->isBackground() && w.loaded(1)->isBackground());
        // File › New: a new tab just after the one in front.
        w.active().model().newProject();
        QCOMPARE(w.count(), 3);
        QCOMPARE(w.activeIndex(), 1);
        QCOMPARE(w.title(1), QStringLiteral("Untitled"));
        QCOMPARE(w.title(2), QStringLiteral("Beta"));
        w.selectNext();
        QCOMPARE(w.activeIndex(), 2);
        w.selectNext();
        QCOMPARE(w.activeIndex(), 0);
        w.selectPrevious();
        QCOMPARE(w.activeIndex(), 2);
        // Closing tabs: the one in front goes, the next comes forward.
        w.close(2);
        QCOMPARE(w.count(), 2);
        QCOMPARE(w.activeIndex(), 1);
        w.closeOthers(1);
        QCOMPARE(w.count(), 1);
        QCOMPARE(w.title(0), QStringLiteral("Untitled"));
        // Ctrl+W on the only tab closes its project, leaving an empty Untitled one.
        w.open(b);
        QCOMPARE(w.count(), 1);
        QVERIFY(w.canClose());
        w.closeActive();
        QCOMPARE(w.count(), 1);
        QVERIFY(w.isBlank(0));
        QVERIFY(w.active().model().project().clips.empty());
        // While Magic works on the tab in front, it stays in front.
        w.newTab();
        bool magic = true;
        Workspace::lockedCheck() = [&magic] { return magic; };
        w.select(0);
        QCOMPARE(w.activeIndex(), 1);
        w.newTab();
        QCOMPARE(w.count(), 2);
        magic = false;
        w.select(0);
        QCOMPARE(w.activeIndex(), 0);
        Workspace::lockedCheck() = nullptr;
        EditorModel::openHandler() = nullptr;
        EditorModel::newTabHandler() = nullptr;
    }

    void reopenAtLaunch() {
        // Tabs come back at launch, in order, with the one in front; ones behind aren't loaded until shown.
        QSettings().remove(QLatin1String(Workspace::tabsKey));
        const QString a = project(QStringLiteral("Gamma"), 1), b = project(QStringLiteral("Delta"), 3);
        {
            Workspace w(true);
            w.open(a);
            w.open(b);
            w.select(0);
            w.autosaveAll();
        }
        Workspace w(true);
        QCOMPARE(w.count(), 2);
        QCOMPARE(w.activeIndex(), 0);
        QVERIFY(!w.loaded(1));
        QCOMPARE(w.title(1), QStringLiteral("Delta"));
        QCOMPARE(w.openPath(1), QFileInfo(b).absoluteFilePath());
        QCOMPARE(w.active().model().title(), QStringLiteral("Gamma"));
        QVERIFY(!w.loaded(1));
        w.select(1);
        QVERIFY(w.loaded(1));
        QCOMPARE(w.loaded(1)->model().project().clips.size(), size_t(3));
        // Each tab its own recovery copy.
        QVERIFY(w.loaded(0)->model().recoveryPath() != w.loaded(1)->model().recoveryPath());
        // A closed tab forgets what it kept.
        const QString gone = w.slot(1);
        w.close(1);
        QVERIFY(!EditorModel::savedSession(gone));
        QCOMPARE(QSettings().value(QLatin1String(Workspace::tabsKey)).toStringList().size(), 1);
    }

    void strip() {
        Workspace w(false);
        MainWindow win(w);
        win.resize(1480, 940);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        auto* strip = win.findChild<TabStrip*>();
        QVERIFY(strip);
        QCOMPARE(strip->items().size(), size_t(1));
        // An empty only tab: no ×.
        QVERIFY(!strip->items()[0].closable);
        QTest::mouseClick(strip, Qt::LeftButton, {}, strip->plusBox().center().toPoint());
        QCOMPARE(w.count(), 2);
        QCOMPARE(strip->items().size(), size_t(2));
        QVERIFY(strip->items()[0].closable && strip->items()[1].closable);
        // Clicking a tab brings it to the front, and the window builds its editor.
        Controller* before = &win.controller();
        QTest::mouseClick(strip, Qt::LeftButton, {}, strip->items()[0].box.center().toPoint());
        QCOMPARE(w.activeIndex(), 0);
        QVERIFY(&win.controller() != before);
        // Its ×.
        QTest::mousePress(strip, Qt::LeftButton, {}, strip->items()[1].close.center().toPoint());
        QTest::mouseRelease(strip, Qt::LeftButton, {}, strip->items()[1].close.center().toPoint());
        QCOMPARE(w.count(), 1);
        // More than fit: the rest in "N more tabs".
        for (int i = 0; i < 20; ++i) w.newTab();
        QApplication::processEvents();
        QVERIFY(strip->hiddenCount() > 0);
        bool frontShown = false;
        for (const auto& it : strip->items()) if (it.index == w.activeIndex()) frontShown = true;
        QVERIFY(frontShown);
        for (const auto& it : strip->items()) QVERIFY(it.box.width() >= TabStrip::minTab - 0.01);
        // Ctrl+T and Ctrl+W through the menus.
        const int n = w.count();
        QTest::keyClick(&win, Qt::Key_T, Qt::ControlModifier);
        QCOMPARE(w.count(), n + 1);
        QTest::keyClick(&win, Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(w.count(), n);
    }

    void pasteAcross() {
        // The Mac's PASTE ACROSS: pasting into another project brings the media too, once, linked to the same file;
        // media it already has (the same file) is used, not added again; clips keep their tracks by place (V3 stays V3,
        // made there if need be). Within the same project it pastes as before.
        Project src;
        const Uuid v3 = src.addTrack(TrackKind::video);
        MediaAsset asset;
        asset.path = QStringLiteral("/tmp/editroom-selftest/shared.mov");
        asset.name = QStringLiteral("shared.mov");
        asset.kind = MediaKind::video;
        asset.duration = 10;
        asset.hasAudio = true;
        src.assets = {asset};
        const Uuid link = newUuid();
        Clip v(v3, ClipKind::video, 2, 3); v.assetID = asset.id; v.linkID = link;
        Clip s(src.audioTracks[0].id, ClipKind::audio, 2, 3); s.assetID = asset.id; s.linkID = link;
        src.clips = {v, s};
        EditorModel a, b;
        a.load(src);
        b.load(Project());
        a.setSelection({v.id});
        a.copySelection();
        b.paste();
        b.seek(20);
        b.paste();
        const Project& q = b.project();
        QCOMPARE(q.assets.size(), size_t(1));
        QCOMPARE(q.assets[0].path, asset.path);
        QCOMPARE(q.videoTracks.size(), size_t(3));
        for (const auto& c : q.clips) if (c.kind == ClipKind::video) QCOMPARE(c.trackID, q.videoTracks[2].id);
        QCOMPARE(q.clips.size(), size_t(4));
        for (const auto& c : q.clips) QCOMPARE(c.assetID, std::optional<Uuid>(q.assets[0].id));
        // Already in Media there (its own id): that one's used.
        Project other;
        MediaAsset mine = asset;
        mine.id = newUuid();
        other.assets = {mine};
        EditorModel c;
        c.load(other);
        c.paste();
        QCOMPARE(c.project().assets.size(), size_t(1));
        QCOMPARE(c.project().clips.size(), size_t(2));
        for (const auto& x : c.project().clips) QCOMPARE(x.assetID, std::optional<Uuid>(mine.id));
        // Within the same project it pastes as before.
        a.seek(10);
        a.paste();
        QCOMPARE(a.project().assets.size(), size_t(1));
        QCOMPARE(a.project().clips.size(), size_t(4));
        QCOMPARE(a.project().videoTracks.size(), size_t(3));
        // One undo takes the pasted clips away and keeps the media (the library isn't undone).
        b.undo();
        QCOMPARE(b.project().clips.size(), size_t(2));
        QCOMPARE(b.project().assets.size(), size_t(1));
    }
};

QTEST_MAIN(TestTabs)
#include "tst_tabs.moc"
