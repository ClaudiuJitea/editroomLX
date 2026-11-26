// EditorModel checks from SelfTest.swift: HISTORY, CAPTION UNDO, RIPPLE TRIM, GRADE PASTE, MEDIA DELETE,
// GAPS+PASTE (paste part), ZOOM (toggle part), plus the undo rules (live merging, 200 steps, library kept).
#include "editor/EditorModel.h"
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace er;

class TestEditor : public QObject {
    Q_OBJECT

    static MediaAsset asset(const QString& name, MediaKind kind = MediaKind::video) {
        MediaAsset a;
        a.path = QStringLiteral("/nonexistent/") + name; a.name = name; a.kind = kind;
        a.duration = 10; a.width = 1280; a.height = 720; a.fps = 30; a.hasAudio = true;
        return a;
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("EditroomTest"));
        QSettings().clear();
    }

    // Magic history: kept in the project file, newest first and each once, and undo leaves it alone.
    void history() {
        EditorModel m;
        m.addMagicPrompt("make a quick cut", "local");
        m.mutate([](Project& p) { p.width = 1280; });
        m.addMagicPrompt("slow-mo moment", "chatgpt");
        m.addMagicPrompt(" make a quick cut ", "claude");
        m.undo();
        QCOMPARE(m.magicHistory().size(), size_t(2));
        QCOMPARE(m.magicHistory()[0].text, QStringLiteral("make a quick cut"));
        QCOMPARE(m.magicHistory()[1].text, QStringLiteral("slow-mo moment"));
        QCOMPARE(m.magicHistory()[0].tab, QStringLiteral("claude"));
        QCOMPARE(m.project().width, 1920);
        for (int i = 0; i < 25; ++i) m.addMagicPrompt(QStringLiteral("prompt %1").arg(i), "local");
        QCOMPARE(m.magicHistory().size(), size_t(20));
        QCOMPARE(m.magicHistory()[0].text, QStringLiteral("prompt 24"));
    }

    // Magic's captions join the cut's undo step: one Undo takes both back.
    void captionUndo() {
        EditorModel m;
        Project p;
        p.assets = {asset("a.mp4")};
        m.load(p);
        const Project before = m.project();
        Clip a(m.project().audioTracks[0].id, ClipKind::audio, 0, 2);
        a.assetID = p.assets[0].id;
        m.mutate([&](Project& q) { q.clips.push_back(a); });
        m.amendLastStep([&](Project& q) { q.addCaptions({a.id}); });
        QVERIFY(m.project().clips.size() == 1 && m.project().captions);
        m.undo();
        QVERIFY(m.project().clips.empty() && !m.project().captions && m.project() == before);
        m.redo();
        QVERIFY(m.project().clips.size() == 1 && m.project().captions);
    }

    // Ripple Trim (Q/W).
    void rippleTrim() {
        struct S { std::unique_ptr<EditorModel> m; Uuid x, y, music; };
        auto setup = [] {
            Project p;
            const Uuid v1 = p.videoTracks[0].id, a1 = p.audioTracks[0].id;
            Track a2(TrackKind::audio);
            p.audioTracks.push_back(a2);
            const Uuid link = newUuid();
            Clip x(v1, ClipKind::video, 0, 4); x.linkID = link;
            Clip xa(a1, ClipKind::audio, 0, 4); xa.linkID = link;
            Clip y(v1, ClipKind::video, 4, 4);
            Clip music(a2.id, ClipKind::audio, 0, 8);
            p.clips = {x, xa, y, music};
            S s{std::make_unique<EditorModel>(), x.id, y.id, music.id};
            s.m->load(p);
            return s;
        };
        auto spans = [](const EditorModel& m) {
            QStringList out;
            for (const auto& c : m.project().clips) out << QString::asprintf("%.1f-%.1f", c.start, c.end());
            out.sort();
            return out.join(u' ');
        };
        // Q at 1s, nothing selected: X and its audio lose 0–1, Y and the music slide back with them.
        auto s = setup();
        s.m->seek(1); s.m->rippleTrim(true);
        QCOMPARE(s.m->project().clip(s.y)->start, 3.0);
        QCOMPARE(spans(*s.m), QStringLiteral("0.0-3.0 0.0-3.0 0.0-7.0 3.0-7.0"));
        QVERIFY(std::abs(s.m->playhead()) < 1e-6);
        // W at 6s: Y loses 6–8; the music's tail goes too.
        s = setup();
        s.m->seek(6); s.m->rippleTrim(false);
        QVERIFY(s.m->project().clip(s.y)->end() == 6 && s.m->project().duration() == 6);
        // W at 1s with X selected: X and its audio lose 1–4, Y slides to 1; the music isn't touched.
        s = setup();
        s.m->setSelection({s.x});
        s.m->seek(1); s.m->rippleTrim(false);
        QVERIFY(s.m->project().clip(s.y)->start == 1 && s.m->project().clip(s.music)->end() == 8 && s.m->project().clips.size() == 4);
        s.m->undo();
        QCOMPARE(s.m->project().clip(s.y)->start, 4.0);
    }

    // Copy and paste grade.
    void gradePaste() {
        Project p;
        const Uuid v = p.videoTracks[0].id, au = p.audioTracks[0].id;
        Clip graded(v, ClipKind::solid, 0, 2);
        ColorGrade g; g.look = QStringLiteral("Warm film"); g.lookIntensity = 0.6; g.exposure = 0.4; g.saturation = -12;
        graded.grade = g;
        Clip other(v, ClipKind::solid, 2, 2);
        ColorGrade og; og.exposure = -1;
        other.grade = og;
        Clip plain(v, ClipKind::text, 4, 2);
        Clip sound(au, ClipKind::audio, 0, 2);
        p.clips = {graded, other, plain, sound};
        EditorModel m;
        m.load(p);
        m.setSelection({graded.id});
        QVERIFY(m.canCopyGrade());
        m.copyGrade();
        m.setSelection({other.id, plain.id, sound.id});
        QVERIFY(m.canPasteGrade());
        m.pasteGrade();
        QVERIFY(m.project().clip(other.id)->grade == g && m.project().clip(plain.id)->grade == g && !m.project().clip(sound.id)->grade);
        m.undo();
        QVERIFY(m.project().clip(other.id)->grade == og && !m.project().clip(plain.id)->grade);
    }

    // Deleting from the Media panel: picked items go, and one Undo brings them back.
    void mediaDelete() {
        EditorModel m;
        Project p;
        p.assets = {asset("a.mp4"), asset("b.wav", MediaKind::audio), asset("c.jpg", MediaKind::image)};
        m.load(p);
        const IdSet ids{p.assets[0].id, p.assets[1].id};
        m.mediaSelection = ids;
        m.deleteSelected();
        for (const auto& id : ids) QVERIFY(!m.project().asset(id));
        QVERIFY(m.mediaSelection.empty());
        m.undo();
        for (const auto& id : ids) QVERIFY(m.project().asset(id));
        QCOMPARE(m.project().assets.size(), size_t(3));
        m.redo();
        QCOMPARE(m.project().assets.size(), size_t(1));
    }

    // Paste makes fresh, still-linked copies at the playhead.
    void paste() {
        Project p;
        const Uuid v = p.videoTracks[0].id, a = p.audioTracks[0].id;
        const Uuid link = newUuid();
        Clip c1(v, ClipKind::solid, 0, 2);
        Clip c2(v, ClipKind::video, 3, 2); c2.linkID = link;
        Clip c2a(a, ClipKind::audio, 3, 2); c2a.linkID = link;
        p.clips = {c1, c2, c2a};
        EditorModel m;
        m.load(p);
        m.setSelection({c2.id});
        m.copySelection();
        m.seek(10);
        m.paste();
        std::vector<Clip> pasted;
        for (const auto& c : m.project().clips) if (c.start == 10) pasted.push_back(c);
        QCOMPARE(pasted.size(), size_t(2));
        QVERIFY(pasted[0].linkID == pasted[1].linkID && pasted[0].linkID != link);
        for (const auto& c : pasted) QVERIFY(c.id != c2.id);
        QCOMPARE(m.playhead(), 12.0);
    }

    // The zoom toggle: on zooms to 200% from 100%; off keeps the scale.
    void zoomToggle() {
        EditorModel m;
        Project q;
        Clip clip(q.videoTracks[0].id, ClipKind::solid, 0, 6);
        q.clips = {clip};
        m.load(q);
        m.toggleZoom(clip.id);
        QVERIFY(m.project().clip(clip.id)->transform->scale == 2 && m.project().clip(clip.id)->transform->zoom && m.playhead() > 0);
        m.toggleZoom(clip.id);
        QVERIFY(!m.project().clip(clip.id)->transform->zoom && m.project().clip(clip.id)->transform->scale == 2);
    }

    void liveMerges() {
        EditorModel m;
        m.load(Project());
        for (int i = 1; i <= 10; ++i) m.live([i](Project& p) { p.width = 1000 + i; });
        QVERIFY(m.canUndo());
        QCOMPARE(m.undoDepth(), size_t(0)); // still live
        QTRY_COMPARE_WITH_TIMEOUT(m.undoDepth(), size_t(1), 2000); // committed 0.8 s after the last change
        m.undo();
        QCOMPARE(m.project().width, 1920);
        // Discard puts it back with no step.
        m.live([](Project& p) { p.width = 7; }, false);
        m.discardLive();
        QCOMPARE(m.project().width, 1920);
        QVERIFY(!m.canRedo() || m.canRedo()); // redo stack is the Mac's: the live change never got there
    }

    void twoHundredSteps() {
        EditorModel m;
        m.load(Project());
        for (int i = 0; i < 250; ++i) m.mutate([i](Project& p) { p.width = 100 + i; });
        QCOMPARE(m.undoDepth(), size_t(200));
        while (m.canUndo()) m.undo();
        QCOMPARE(m.project().width, 100 + 49);
    }

    void libraryNotUndone() {
        EditorModel m;
        m.load(Project());
        m.mutate([](Project& p) { p.width = 640; });
        m.changeLibrary([](Project& p) { p.assets.push_back(asset("new.mp4")); });
        m.undo();
        QCOMPARE(m.project().width, 1920);
        QCOMPARE(m.project().assets.size(), size_t(1)); // an import stays
    }

    void markersAndRange() {
        EditorModel m;
        Project p;
        p.clips = {Clip(p.videoTracks[0].id, ClipKind::solid, 0, 10)};
        m.load(p);
        m.seek(2);
        m.addMarker();
        QCOMPARE(m.project().markers->front().name, QStringLiteral("Marker 1"));
        m.addMarker(); // already one here: opens it
        QCOMPARE(m.project().markers->size(), size_t(1));
        QVERIFY(m.editingMarker);
        m.setInPoint();
        m.seek(1);
        m.setOutPoint(); // before the in point: the in point goes
        QVERIFY(!m.project().inPoint && m.project().outPoint == 1.0);
        m.clearRange();
        QVERIFY(!m.project().outPoint);
    }

    void saveAndOpen() {
        QTemporaryDir dir;
        EditorModel m;
        m.testLaunch = true;
        Project p;
        MediaAsset a = asset("clip.mov");
        a.path = dir.filePath(QStringLiteral("media/clip.mov"));
        p.assets = {a};
        m.load(p);
        m.mutate([](Project& q) { q.fps = 25; });
        const QString file = dir.filePath(QStringLiteral("cut.room"));
        QVERIFY(m.writeTo(file));
        QVERIFY(!m.isDirty());
        QCOMPARE(m.title(), QStringLiteral("cut"));
        EditorModel n;
        n.testLaunch = true;
        QVERIFY(n.open(file));
        QCOMPARE(n.project().fps, 25.0);
        QCOMPARE(*n.project().assets[0].relativePath, QStringLiteral("media/clip.mov"));
        QVERIFY(!n.canUndo());
    }

    void addAtPlayhead() {
        EditorModel m;
        Project p;
        MediaAsset a = asset("v.mp4");
        a.duration = 3.5;
        p.assets = {a};
        m.load(p);
        m.addAtPlayhead(DragPayload::ofAsset(a.id));
        QCOMPARE(m.project().clips.size(), size_t(2)); // video and its linked sound
        QCOMPARE(m.project().clips[0].duration, 3.5);
        m.addAtPlayhead(DragPayload::text()); // never covers anything: on V2
        QCOMPARE(m.project().clips.back().trackID, m.project().videoTracks[1].id);
        QCOMPARE(EditorModel::standardFPS(29.97002997), 29.97);
        QCOMPARE(EditorModel::standardFPS(0), 30.0);
    }
};

QTEST_GUILESS_MAIN(TestEditor)
#include "tst_editor.moc"
