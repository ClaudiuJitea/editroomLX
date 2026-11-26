// Local Magic: LocalBrain reading requests, AutoPlanner and the Choreographer on made-up analyses (whole beats,
// cuts on the song's beats, ending on a bar), and a real run on the test clips: a new cut from the library, then
// the edit improved, each one undo step.
#include "mcp/LocalImprover.h"
#include "mcp/LocalMagic.h"
#include "mcp/LocalPlanners.h"
#include "mcp/MentionField.h"
#include "mcp/MagicShow.h"
#include "mcp/MagicThinking.h"
#include "ui/TimelineCanvas.h"
#include <QListWidget>
#include "ui/Controller.h"
#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace er;

namespace {
MediaAsset asset(const QString& name, MediaKind kind, double duration, bool audio = true) {
    MediaAsset a;
    a.id = newUuid();
    a.name = name;
    a.kind = kind;
    a.duration = duration;
    a.hasAudio = audio;
    a.width = 1920;
    a.height = 1080;
    return a;
}
ClipAnalysis analysis(double duration, double created, double quality) {
    ClipAnalysis a;
    a.duration = duration;
    a.created = created;
    for (double t = 0.125; t < duration; t += 0.25)
        a.samples.push_back({t, 0.45, 50 + quality * 100 * (0.5 + 0.5 * std::sin(t)), 0.01, 0.4 + quality * 0.2, 0, std::nullopt, std::nullopt});
    return a;
}
} // namespace

class TestLocal : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
    }

    void reading() {
        const auto none = [](const Uuid&) { return std::optional<SceneTags>(); };
        auto r = LocalBrain::recipe(QStringLiteral("make it 30 seconds, fast and black and white, vertical for tiktok"), {}, {}, false, none);
        QCOMPARE(r.seconds, std::optional<double>(30));
        QVERIFY(r.pace == LocalRecipe::Pace::fast);
        QCOMPARE(r.look, std::optional<QString>(QStringLiteral("Mono")));
        QVERIFY(r.frame == LocalRecipe::Frame::vertical);
        r = LocalBrain::recipe(QStringLiteral("slow motion please"), {}, {}, false, none);
        QCOMPARE(r.speed, std::optional<double>(0.5));
        QVERIFY(r.pace == LocalRecipe::Pace::unchanged);
        r = LocalBrain::recipe(QStringLiteral("no music, 1:30 long"), {}, {}, false, none);
        QVERIFY(r.music == LocalRecipe::Music::off);
        QCOMPARE(r.seconds, std::optional<double>(90));
        r = LocalBrain::recipe(QStringLiteral("use every clip, at least twelve shots"), {}, {}, false, none);
        QVERIFY(r.every && r.wholeLibrary);
        QCOMPARE(r.shots, std::optional<int>(12));
        r = LocalBrain::recipe(QStringLiteral("add a title saying \"Summer\" at the end"), {}, {}, false, none);
        QCOMPARE(r.title, std::optional<QString>(QStringLiteral("Summer")));
        QVERIFY(r.titleAtEnd);
        // Named media: "only IMG_1607".
        const auto a = asset(QStringLiteral("IMG_1607.MOV"), MediaKind::video, 10), b = asset(QStringLiteral("IMG_1608.MOV"), MediaKind::video, 10);
        r = LocalBrain::recipe(QStringLiteral("only use 1607"), {a, b}, {}, false, none);
        QCOMPARE(r.keepOnly, std::set<Uuid>{a.id});
        // Nothing Local can do.
        r = LocalBrain::recipe(QStringLiteral("what is the capital of France?"), {}, {}, false, none);
        QVERIFY(r.isEmpty());
        QVERIFY(LocalBrain::asksJudgement(QStringLiteral("should I add sound effects?")));
    }

    void plansToTheBeat() {
        Project p;
        std::vector<std::pair<MediaAsset, ClipAnalysis>> clips;
        for (int i = 0; i < 4; ++i) {
            auto a = asset(QStringLiteral("clip%1.mp4").arg(i), MediaKind::video, 20);
            p.assets.push_back(a);
            clips.push_back({a, analysis(20, 1000 + i, 0.3 + 0.2 * i)});
        }
        auto song = asset(QStringLiteral("song.wav"), MediaKind::audio, 60, true);
        p.assets.push_back(song);
        MusicAnalysis m;
        m.duration = 60;
        m.bpm = 120;
        for (double t = 0.2; t < 60; t += 0.5) { m.beats.push_back(t); m.energy.push_back(float(0.5 + 0.3 * std::sin(t / 4))); }
        m.barPhase = 0;
        auto d = AutoPlanner::decide(p, clips, std::make_pair(song, m));
        QVERIFY(d && !d->shots.empty() && d->music);
        // Every shot is whole beats long; the cut is whole bars.
        double total = 0;
        for (const auto& s : d->shots) {
            QVERIFY2(std::abs(std::round(s.duration / 0.5) - s.duration / 0.5) < 1e-6, qPrintable(QString::number(s.duration)));
            total += s.duration;
        }
        QVERIFY(std::abs(std::fmod(total + 1e-9, 2.0)) < 1e-6);
        // In the order filmed, never one clip twice in a row.
        for (size_t i = 1; i < d->shots.size(); ++i) QVERIFY(d->shots[i].asset.id != d->shots[i - 1].asset.id);
        // The plan builds it: shots butted on V1 from 0, the song on A3.
        auto plan = Choreographer::plan(p, *d);
        QVERIFY(plan);
        const Project out = plan->result();
        std::vector<Clip> v1;
        for (const auto& c : out.clips) if (c.trackID == out.videoTracks[0].id) v1.push_back(c);
        std::sort(v1.begin(), v1.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        QCOMPARE(v1.size(), d->shots.size());
        QCOMPARE(v1.front().start, 0.0);
        for (size_t i = 1; i < v1.size(); ++i) QVERIFY(std::abs(v1[i].start - v1[i - 1].end()) < 1.0 / 30 + 1e-9);
        QVERIFY(std::any_of(out.clips.begin(), out.clips.end(), [&](const Clip& c) { return c.assetID == song.id && c.trackID == out.audioTracks[2].id; }));
        // The same media gives the same cut.
        auto again = Choreographer::plan(p, *AutoPlanner::decide(p, clips, std::make_pair(song, m)));
        QCOMPARE(again->tasks.size(), plan->tasks.size());

        // Improving it: a shorter version, 8 seconds.
        std::map<Uuid, ClipAnalysis> analyses;
        for (const auto& [a, an] : clips) analyses.emplace(a.id, an);
        LocalRecipe shorter;
        shorter.seconds = 8;
        auto o = LocalImprover::plan(out, analyses, m, shorter);
        QVERIFY(o && !o->plan.tasks.empty());
        const Project improved = o->plan.result();
        double end = 0;
        for (const auto& c : improved.clips) if (c.trackID == improved.videoTracks[0].id) end = std::max(end, c.end());
        QVERIFY2(end < 12, qPrintable(QString::number(end) + " " + o->summary));
    }

    void runsOnTheTestClips() {
        const QString dir = QStringLiteral(ER_TEST_MEDIA);
        if (!QFile::exists(dir + QStringLiteral("/a_main.mp4"))) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Controller c;
        for (const char* f : {"a_main.mp4", "sdr_h264.mp4", "rotated_h264.mp4", "photo.jpg"})
            if (auto a = c.media().importFile(dir + u'/' + QLatin1String(f))) c.model().changeLibrary([&](Project& p) { p.assets.push_back(*a); });
        LocalMagic& local = LocalMagic::of(c);
        QSignalSpy spy(&local, &LocalMagic::finished);
        local.start(LocalRecipe{});
        QVERIFY(spy.wait(60000));
        QVERIFY2(spy.first().at(1).toBool(), qPrintable(spy.first().at(0).toString()));
        QVERIFY(spy.first().at(0).toString().startsWith(QStringLiteral("Made a ")));
        const size_t made = c.model().project().clips.size();
        QVERIFY(made > 0);
        // Again, with the edit there: improved (or left as it is), never a crash; one undo takes it back.
        local.run(QStringLiteral("make it black and white"));
        QVERIFY(spy.wait(60000));
        QTest::qWait(100);
        // Once: one note.
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("One look (Mono)."));
        {
            for (const auto& cl : c.model().project().clips)
                if (cl.kind == ClipKind::video || cl.kind == ClipKind::image) QCOMPARE(cl.grade->look, std::optional<QString>(QStringLiteral("Mono")));
            c.model().undo();
            QCOMPARE(c.model().project().clips.size(), made);
        }
    }

    /// A small edit: three shots butted on V1, each with its sound on A1.
    static Project threeShots(std::vector<MediaAsset>& assets) {
        Project p;
        double t = 0;
        for (const char* n : {"beach.mp4", "dog.mp4", "cake.mp4", "spare.mp4"}) {
            auto a = asset(QLatin1String(n), MediaKind::video, 10);
            p.assets.push_back(a);
            assets.push_back(a);
            if (QLatin1String(n) == QLatin1String("spare.mp4")) continue;
            const Uuid link = newUuid();
            Clip v(p.videoTracks[0].id, ClipKind::video, t, 3, 2);
            v.assetID = a.id;
            v.linkID = link;
            Clip s(p.audioTracks[0].id, ClipKind::audio, t, 3, 2);
            s.assetID = a.id;
            s.linkID = link;
            p.clips.push_back(v);
            p.clips.push_back(s);
            t += 3;
        }
        return p;
    }
    static std::vector<QString> order(const Project& p) {
        std::vector<Clip> v;
        for (const auto& c : p.clips) if (c.trackID == p.videoTracks[0].id) v.push_back(c);
        std::sort(v.begin(), v.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        std::vector<QString> out;
        for (const auto& c : v) out.push_back(p.asset(c.assetID)->name);
        return out;
    }

    void planners() {
        std::vector<MediaAsset> as;
        const Project p = threeShots(as);
        const MagicPointers ptr{4.5, {}, std::nullopt};
        // Mentions and typed times.
        const auto found = MagicMention::find(QStringLiteral("put @cake in front of @beach.mp4, @playhead"), p.assets);
        QCOMPARE(found.size(), size_t(3));
        QCOMPARE(found[0].mention.id, as[2].id);
        QVERIFY(found[2].mention.kind == MagicMention::Kind::playhead);
        QCOMPARE(MagicMention::plain(QStringLiteral("trim @selected"), p.assets), QStringLiteral("trim the selected clips"));
        QCOMPARE(MagicMention::clock(72.4), QStringLiteral("1:12.4"));
        auto span = MagicTimes::span(QStringLiteral("between 0:05 and 0:07.5"), 30);
        QVERIFY(span && span->start == 5 && span->end == 7.5);
        QVERIFY(!MagicTimes::span(QStringLiteral("make it 5s to 8s"), 30));
        QVERIFY(MagicTimes::span(QStringLiteral("from 5s to 8s"), 30));
        // Reordering by name.
        auto moved = LocalPointed::reorder(QStringLiteral("put @cake in front of @beach"), p);
        QVERIFY(moved && moved->plan);
        QCOMPARE(order(moved->plan->result()), (std::vector<QString>{"cake.mp4", "beach.mp4", "dog.mp4"}));
        QCOMPARE(moved->said, QStringLiteral("Put cake.mp4 in front of beach.mp4."));
        // A title at the end.
        QVERIFY(LocalTitle::asks(QStringLiteral("add a title saying \"The End\" at the end"), p));
        auto title = LocalTitle::plan(QStringLiteral("add a title saying \"The End\" at the end"), 0, p);
        const Project titled = title.plan->result();
        const auto text = std::find_if(titled.clips.begin(), titled.clips.end(), [](const Clip& c) { return c.kind == ClipKind::text; });
        QVERIFY(text != titled.clips.end() && text->text.string == QStringLiteral("The End") && std::abs(text->end() - 9) < 0.05);
        // One more clip at the start: everything moves along.
        QVERIFY(LocalAdd::asks(QStringLiteral("add one more clip at the start"), p));
        auto added = LocalAdd::plan(QStringLiteral("add one more clip at the start"), QStringLiteral("add one more clip at the start"), ptr, p);
        QCOMPARE(order(added.plan->result()).front(), QStringLiteral("spare.mp4"));
        // Split at the playhead; end there.
        auto split = LocalPointed::plan(QStringLiteral("split at the playhead"), {}, ptr, p);
        QVERIFY(split && split->plan);
        QCOMPARE(split->plan->result().clips.size(), p.clips.size() + 2);
        auto ended = LocalPointed::plan(QStringLiteral("end it at the playhead"), {}, ptr, p);
        QVERIFY(std::abs(ended->plan->result().duration() - 4.5) < 0.05);
        // The selected clip taken out, gap closed.
        MagicPointers sel = ptr;
        sel.selected = {p.clips[2].id};
        auto removed = LocalPointed::plan(QStringLiteral("remove this clip"), {}, sel, p);
        QCOMPARE(order(removed->plan->result()), (std::vector<QString>{"beach.mp4", "cake.mp4"}));
        QVERIFY(std::abs(removed->plan->result().duration() - 6) < 0.05);
        // A slow-mo moment in the shot under the playhead.
        QVERIFY(LocalRamp::asks(QStringLiteral("add a slow-mo moment")));
        auto ramp = LocalRamp::plan(QStringLiteral("add a slow-mo moment"), ptr, p);
        QVERIFY(ramp.plan && ramp.said.startsWith(QStringLiteral("Slowed a moment in the shot to 50%")));
        // Pauses: one stretch of talk in the middle of each shot's file part.
        auto talk = [](const Uuid&) {
            Transcript t;
            t.words = {{2.8, 3.2, QStringLiteral("hello")}, {3.3, 3.6, QStringLiteral("there")}};
            return std::optional<Transcript>(t);
        };
        auto pauses = LocalSpeech::plan(p, std::nullopt, talk);
        QVERIFY2(pauses.plan, qPrintable(pauses.said));
        QVERIFY(pauses.plan->result().duration() < 3.5);
    }

    void mentionField() {
        Controller c;
        auto a = asset(QStringLiteral("beach.mov"), MediaKind::video, 10);
        c.model().changeLibrary([&](Project& p) { p.assets.push_back(a); });
        MentionField f(c.model());
        f.show();
        f.setFocus();
        QApplication::setActiveWindow(&f);
        QTest::keyClicks(&f, QStringLiteral("cut @pl"));
        QVERIFY(f.listShown());
        QTest::keyClick(&f, Qt::Key_Return);
        QCOMPARE(f.toPlainText(), QStringLiteral("cut @playhead "));
        QVERIFY(!f.listShown());
        // @media: opens the files; a pick fills in the name.
        QTest::keyClicks(&f, QStringLiteral("and @media:"));
        QVERIFY(f.listShown());
        QTest::keyClick(&f, Qt::Key_Tab);
        QCOMPARE(f.toPlainText(), QStringLiteral("cut @playhead and @beach.mov "));
        // The options: "selected" is offered but greyed out with nothing selected.
        const auto o = MentionOption::list(QString(), c.model());
        QVERIFY(!o.empty() && o.front().name == QStringLiteral("selected") && !o.front().enabled);
        // Return with the list closed sends.
        QSignalSpy sent(&f, &MentionField::submitted);
        QTest::keyClick(&f, Qt::Key_Return);
        QCOMPARE(sent.count(), 1);
    }

    /// With a timeline on screen the cut is played by cursors, live, and is one undo step; Esc puts it all back.
    void cursorShow() {
        Controller c;
        std::vector<MediaAsset> as;
        const Project p = threeShots(as);
        c.model().mutate([&](Project& q) { q = p; });
        TimelineCanvas canvas(c);
        canvas.resize(1000, 300);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto moved = LocalPointed::reorder(QStringLiteral("swap @beach and @cake"), c.model().project());
        QVERIFY(moved && moved->plan);
        auto title = LocalTitle::plan(QStringLiteral("add a title"), 0, c.model().project());
        bool finished = false, over = false;
        MagicShow::play(c, *title.plan, [&](bool f) { finished = f; over = true; });
        QVERIFY(MagicShow::running());
        if (const QString dir = qEnvironmentVariable("ER_SHOTS"); !dir.isEmpty()) {
            QTest::qWait(250);
            canvas.window()->grab().save(dir + QStringLiteral("/magic-show.png"));
        }
        QTRY_VERIFY_WITH_TIMEOUT(over, 10000);
        QVERIFY(finished);
        QVERIFY(std::any_of(c.model().project().clips.begin(), c.model().project().clips.end(), [](const Clip& x) { return x.kind == ClipKind::text; }));
        QTRY_VERIFY(!MagicShow::running());
        c.model().undo();
        QCOMPARE(c.model().project().clips.size(), p.clips.size());
        // Stopped partway: nothing changed.
        over = false;
        MagicShow::play(c, *title.plan, [&](bool f) { finished = f; over = true; });
        QTest::qWait(60);
        MagicShow::stopAll();
        QVERIFY(over && !finished);
        QCOMPARE(c.model().project().clips.size(), p.clips.size());
    }

    /// The thinking scene: Looking, Analyzing, Thinking (with the cut sketched), the editor locked meanwhile; Esc
    /// cancels. Then a whole Local run through it: scene, sketch, cursors, one undo step.
    void thinkingScene() {
        const QString dir = QStringLiteral(ER_TEST_MEDIA);
        if (!QFile::exists(dir + QStringLiteral("/a_main.mp4"))) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Controller c;
        QWidget window;
        auto* canvas = new TimelineCanvas(c, &window);
        canvas->setGeometry(0, 0, 1000, 300);
        window.resize(1000, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        MagicThinking* t = MagicThinking::begin(c, QStringLiteral("Local"), Qt::white);
        QVERIFY(t && MagicThinking::current() == t);
        QCOMPARE(t->stage(), MagicThinking::Stage::looking);
        t->analyze(std::nullopt);
        QCOMPARE(t->stage(), MagicThinking::Stage::analyzing);
        // Locked: a key on the timeline does nothing (the playhead stays).
        const double before = c.model().playhead();
        QTest::keyClick(canvas, Qt::Key_Right);
        QCOMPARE(c.model().playhead(), before);
        std::vector<MediaAsset> as;
        t->plan(threeShots(as));
        QCOMPARE(t->stage(), MagicThinking::Stage::thinking);
        if (const QString shots = qEnvironmentVariable("ER_SHOTS"); !shots.isEmpty()) {
            QTest::qWait(900);
            window.grab().save(shots + QStringLiteral("/magic-thinking.png"));
        }
        QTest::keyClick(canvas, Qt::Key_Escape);
        QTRY_VERIFY(!MagicThinking::current());

        for (const char* f : {"a_main.mp4", "sdr_h264.mp4", "rotated_h264.mp4"})
            if (auto a = c.media().importFile(dir + u'/' + QLatin1String(f))) c.model().changeLibrary([&](Project& p) { p.assets.push_back(*a); });
        LocalMagic& local = LocalMagic::of(c);
        QSignalSpy spy(&local, &LocalMagic::finished);
        local.start(LocalRecipe{});
        QVERIFY(MagicThinking::current());
        QVERIFY(spy.wait(30000));
        QVERIFY2(spy.first().at(1).toBool(), qPrintable(spy.first().at(0).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(!MagicThinking::current() && !MagicShow::running(), 5000);
        QVERIFY(!c.model().project().clips.empty());
        c.model().undo();
        QVERIFY(c.model().project().clips.empty());
    }

    void localZoom() {
        // "@selected add zoom animation" zooms the selected shot (and adds no clip); "zoom in and out" at the
        // playhead; "1.5x" sets how far; "remove the zoom" takes it off and keeps the scale.
        Project p;
        Clip a(p.videoTracks[0].id, ClipKind::solid, 0, 4), b(p.videoTracks[0].id, ClipKind::solid, 4, 4);
        p.clips = {a, b};
        auto run = [](const QString& text, const MagicPointers& ptr, const Project& on) {
            const LocalOutcome o = LocalZoom::plan(text, ptr, on);
            return o.plan ? o.plan->result() : on;
        };
        QVERIFY(LocalZoom::asks(QStringLiteral("@selected add zoom animation")));
        QVERIFY(!LocalZoom::asks(QStringLiteral("add a clip at the end")));
        MagicPointers sel; sel.playhead = 6; sel.selected = {a.id};
        const Project selected = run(QStringLiteral("@selected add zoom animation"), sel, p);
        QCOMPARE(int(selected.clips.size()), 2);
        QVERIFY(selected.clip(a.id)->transform && selected.clip(a.id)->transform->zoom);
        QVERIFY(selected.clip(a.id)->transform->zoom->mode == ZoomMove::Mode::zoomIn);
        QCOMPARE(selected.clip(a.id)->transform->scale, 2.0);
        QVERIFY(!selected.clip(b.id)->transform);
        MagicPointers none; none.playhead = 6;
        const Project inOut = run(QStringLiteral("zoom in and out 1.5x"), none, p);
        QVERIFY(inOut.clip(b.id)->transform && inOut.clip(b.id)->transform->zoom->mode == ZoomMove::Mode::inOut);
        QCOMPARE(inOut.clip(b.id)->transform->scale, 1.5);
        QVERIFY(!inOut.clip(a.id)->transform);
        MagicPointers at; at.playhead = 1; at.selected = {a.id};
        const Project off = run(QStringLiteral("remove the zoom"), at, selected);
        QVERIFY(off.clip(a.id)->transform && !off.clip(a.id)->transform->zoom);
        QCOMPARE(off.clip(a.id)->transform->scale, 2.0);
    }
};

QTEST_MAIN(TestLocal)
#include "tst_local.moc"
