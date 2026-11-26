// The model checks from the Mac's SelfTest.swift (SPEED GROUP, RAMP, STICKY, TRACKS, ZOOM, TIMECODE,
// TRANSITIONS, GAPS, RANGE, RELINK), with the same inputs and the same expected outputs. Checks that need
// media, the exporter or the EditorModel are ported with those parts later.
#include "model/Codec.h"
#include "model/Project.h"
#include "model/Timecode.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>

using namespace er;

class TestModel : public QObject {
    Q_OBJECT

    static MediaAsset videoAsset(double duration = 10) {
        MediaAsset a;
        a.path = QStringLiteral("/media/a_main.mp4");
        a.name = QStringLiteral("a_main.mp4");
        a.kind = MediaKind::video;
        a.duration = duration;
        a.width = 1280; a.height = 720; a.fps = 30; a.hasAudio = true;
        return a;
    }

private slots:
    // Speed change on several clips: they stay butted together, and untouched clips are pushed, never overlapped.
    void speedGroup() {
        Project p;
        const MediaAsset video = videoAsset();
        p.assets = {video};
        const Uuid t = p.videoTracks[0].id;
        Clip a(t, ClipKind::video, 0, 2); a.assetID = video.id;
        Clip b(t, ClipKind::video, 2, 2, 2); b.assetID = video.id;
        Clip c(t, ClipKind::solid, 4, 1);
        p.clips = {a, b, c};
        Project fast = p, slow = p;
        fast.setSpeed({a.id, b.id}, 2);
        slow.setSpeed({a.id, b.id}, 0.5);
        auto spans = [](const Project& q) { std::vector<std::pair<double, double>> v; for (auto& c : q.clips) v.push_back({c.start, c.end()}); return v; };
        QCOMPARE(spans(fast), (std::vector<std::pair<double, double>>{{0, 1}, {1, 2}, {4, 5}}));
        QCOMPARE(spans(slow), (std::vector<std::pair<double, double>>{{0, 4}, {4, 8}, {8, 9}}));
    }

    // Speed ramps: 100% → 400% → 100% over 4s of footage.
    void ramp() {
        Project p;
        const MediaAsset video = videoAsset();
        p.assets = {video};
        const Uuid t = p.videoTracks[0].id, at = p.audioTracks[0].id;
        const Uuid link = newUuid();
        Clip v(t, ClipKind::video, 0, 4, 0.5); v.assetID = video.id; v.linkID = link;
        Clip a = v; a.id = newUuid(); a.trackID = at; a.kind = ClipKind::audio;
        Clip after(t, ClipKind::solid, 4, 1);
        p.clips = {v, a, after};
        p.setRamp({v.id, a.id}, 4, 0, 0.25, 0.75, 1, 1, 1);
        const Clip rv = *p.clip(v.id), ra = *p.clip(a.id);
        QVERIFY(std::abs(rv.sourceLength() - 4) < 0.02);
        // Pure 400% would be 1s, pure 100% 4s: a ramp at both ends lands in between.
        QVERIFY(rv.duration > 1.2 && rv.duration < 3);
        QVERIFY(std::abs(ra.duration - rv.duration) < 1e-6 && p.clip(after.id)->start >= rv.end() - 1e-6);
        for (double x = 0; x <= 4; x += 0.37) {
            const double s = rv.sourceStart + x;
            const double back = rv.sourceTime(rv.start + rv.timelineSpan(rv.sourceStart, s));
            QVERIFY2(std::abs(back - s) < 1e-4, qPrintable(QString::number(x)));
        }
        QVERIFY(std::abs(rv.speedAtSource(rv.sourceStart) - 1) < 1e-6 && std::abs(rv.speedAtSource(rv.sourceStart + 2) - 4) < 1e-6);
        Project q = p;
        const double mid = rv.start + rv.duration * 0.3;
        q.split({rv.id}, mid);
        std::vector<Clip> halves;
        for (auto& c : q.clips) if (c.trackID == t && c.kind == ClipKind::video) halves.push_back(c);
        std::sort(halves.begin(), halves.end(), [](auto& x, auto& y) { return x.start < y.start; });
        QCOMPARE(halves.size(), size_t(2));
        QVERIFY(std::abs(halves[0].sourceLength() + halves[1].sourceLength() - rv.sourceLength()) < 1e-3);
        QVERIFY(std::abs(halves[1].sourceStart - rv.sourceTime(mid)) < 1e-4);
        QVERIFY(std::abs(halves[1].speedAtSource(halves[1].sourceStart) - rv.speedAtSource(halves[1].sourceStart)) < 1e-9);
        const double length = video.duration;
        const SpeedRamp m = rv.ramp->mirrored(length);
        QVERIFY(std::abs(m.speed(length - (rv.sourceStart + 0.3), 4) - rv.speedAtSource(rv.sourceStart + 0.3)) < 1e-9);
        Project back = p;
        back.setRamp({v.id, a.id}, 4, 0, 0, 1, 1, 1, 1);
        // A slow-motion moment in the middle.
        Project moment = p;
        moment.setRamp({v.id, a.id}, 0.25, 0.3, 0.4, 0.6, 0.7, 1, 1);
        const Clip mc = *moment.clip(v.id);
        const double L = mc.sourceLength(), s0 = mc.sourceStart;
        QVERIFY(std::abs(mc.speedAtSource(s0 + 0.1 * L) - 1) < 1e-6);
        QVERIFY(std::abs(mc.speedAtSource(s0 + 0.5 * L) - 0.25) < 1e-6);
        QVERIFY(std::abs(mc.speedAtSource(s0 + 0.9 * L) - 1) < 1e-6);
        QVERIFY(std::abs(L - rv.sourceLength()) < 0.02);
        QVERIFY(std::abs(mc.duration - L * (0.6 + 0.2 / 0.25 + 0.2 * 2.2)) < 0.6);
        QVERIFY(!back.clip(v.id)->ramp && std::abs(back.clip(v.id)->duration - 1) < 0.04);
    }

    // Sticky edges: three butted clips; shortening the first pulls the other two back, and a linked
    // partner follows its clip.
    void sticky() {
        Project p;
        const Uuid v = p.videoTracks[0].id, a = p.audioTracks[0].id;
        const Uuid link = newUuid();
        Clip c1(v, ClipKind::solid, 0, 4);
        Clip c2(v, ClipKind::solid, 4, 2); c2.linkID = link;
        Clip c3(v, ClipKind::solid, 6, 1);
        Clip c2a(a, ClipKind::solid, 4, 2); c2a.linkID = link;
        Clip far(v, ClipKind::solid, 9, 1);
        p.clips = {c1, c2, c3, c2a, far};
        Clip shortC = c1; shortC.trimTail(3);
        auto carried = p.carried({{c1.id, shortC}});
        QVERIFY(carried.count(c2.id) && carried[c2.id].start == 3);
        QVERIFY(carried.count(c3.id) && carried[c3.id].start == 5);
        QVERIFY(carried.count(c2a.id) && carried[c2a.id].start == 3);
        QVERIFY(!carried.count(far.id));
        // Growing it by 3 pushes the chain into the far clip, which joins in.
        Clip longC = c1; longC.trimTail(7);
        auto pushed = p.carried({{c1.id, longC}});
        QVERIFY(pushed.count(c3.id) && pushed[c3.id].start == 9);
        QVERIFY(pushed.count(far.id) && pushed[far.id].start == 10);
        auto merged = pushed; merged.emplace(c1.id, longC);
        QVERIFY(p.fits(merged));
        // Doubling the speed of a 4s video clip halves it, and the touching clip follows it back.
        Project q;
        const Uuid qv = q.videoTracks[0].id;
        Clip fastClip(qv, ClipKind::video, 0, 4);
        Clip next(qv, ClipKind::solid, 4, 2);
        q.clips = {fastClip, next};
        Project plain = q;
        plain.setSpeed({fastClip.id}, 2);
        q.setSpeed({fastClip.id}, 2, true);
        QCOMPARE(q.clip(fastClip.id)->duration, 2.0);
        QCOMPARE(q.clip(next.id)->start, 2.0);
        QCOMPARE(plain.clip(next.id)->start, 4.0);
    }

    // Tracks: old files without solo/lock still open; solo silences the others; locked clips are left out.
    void tracks() {
        const QByteArray old = R"({"version":1,"project":{"width":1920,"height":1080,"fps":30,"settingsLocked":false,"clips":[],"assets":[],
            "audioTracks":[{"id":"6F1D8C64-1B2E-4C38-9A6E-0B9C1F6A1B2C","kind":"audio","hidden":false,"muted":false,"gainDB":0}],
            "videoTracks":[{"id":"6F1D8C64-1B2E-4C38-9A6E-0B9C1F6A1B2D","kind":"video","hidden":false,"muted":false,"gainDB":0}]}})";
        auto res = codec::decodeDocument(old);
        QVERIFY2(res.document, qPrintable(res.error));
        const Track& t = res.document->project.audioTracks[0];
        QVERIFY(!t.solo && !t.locked && t.duckDB == -12 && !t.duckUnder);
        Project p;
        p.audioTracks[1].solo = true;
        QVERIFY(!p.isAudible(p.audioTracks[0]) && p.isAudible(p.audioTracks[1]));
        p.videoTracks[0].locked = true;
        Clip a(p.videoTracks[0].id, ClipKind::solid, 0, 1);
        Clip b(p.videoTracks[1].id, ClipKind::solid, 0, 1);
        p.clips = {a, b};
        QCOMPARE(p.unlocked({a.id, b.id}), IdSet{b.id});
    }

    // Zoom on Scale (the model parts; the drawn check comes with the compositor).
    void zoom() {
        ZoomMove z = ZoomMove::starter(ZoomMove::Mode::inOut, 8);
        QVERIFY(z.amount(0) == 0 && z.amount(0.5) > 0 && z.amount(0.5) < 1 && z.amount(4) == 1 && z.amount(7.99) < 0.01);
        z.smooth = false;
        QVERIFY(std::abs(z.amount(1) - 0.5) < 1e-9 && z.p2 == 2);
        const ZoomMove zin = ZoomMove::starter(ZoomMove::Mode::zoomIn, 8), zout = ZoomMove::starter(ZoomMove::Mode::zoomOut, 8);
        QVERIFY(zin.amount(7.9) == 1 && zout.amount(0) == 1 && zout.amount(7.99) < 0.01);
        Project p;
        Clip c(p.videoTracks[0].id, ClipKind::solid, 0, 8);
        ClipTransform tr; tr.scale = 2; tr.x = 0.9; tr.y = 0; tr.zoom = ZoomMove::starter(ZoomMove::Mode::inOut, 8);
        c.transform = tr;
        p.clips = {c};
        QCOMPARE(c.transform->at(4)->x, 0.5);
        const IdSet right = p.split({c.id}, 4);
        QCOMPARE(right.size(), size_t(1));
        const ZoomMove l = *p.clip(c.id)->transform->zoom, r = *p.clip(*right.begin())->transform->zoom;
        QVERIFY(l.amount(3.9) == 1 && r.amount(0) == 1 && r.amount(3.99) < 0.01);
    }

    // Typed timecodes.
    void timecode() {
        Timecode::setCurrentStyle(TimeStyle::timecode);
        const std::vector<std::pair<QString, std::optional<double>>> cases = {
            {"1000", 10}, {"1:02:03", 62.1}, {"00:00:02:15", 2.5}, {"12", 0.4}, {"abc", std::nullopt}};
        for (const auto& [s, want] : cases) {
            const auto v = Timecode::parse(s, 30);
            if (!want) QVERIFY2(!v, qPrintable(s));
            else QVERIFY2(v && std::abs(*v - *want) < 1e-9, qPrintable(s));
        }
        QCOMPARE(Timecode::full(5.5, 30), QStringLiteral("00:00:05:15"));
        QCOMPARE(Timecode::simple(5.5), QStringLiteral("0:05.50"));
        QCOMPARE(Timecode::simple(3725.5), QStringLiteral("1:02:05.50"));
        QCOMPARE(Timecode::shortString(65), QStringLiteral("1:05"));
        Timecode::setCurrentStyle(TimeStyle::simple);
        QCOMPARE(*Timecode::parse("1:05", 30), 65.0);
        QCOMPARE(*Timecode::parse("0:05.50", 30), 5.5);
        QCOMPARE(Timecode::string(5.5, 30), QStringLiteral("0:05.50"));
        Timecode::setCurrentStyle(TimeStyle::timecode);
    }

    void decibels() {
        QCOMPARE(formatDB(-60), QStringLiteral("−∞ dB"));
        QCOMPARE(formatDB(1.54), QStringLiteral("+1.5 dB"));
        QCOMPARE(formatDB(-3), QStringLiteral("−3.0 dB"));
        QCOMPARE(formatDB(0.04), QStringLiteral("0.0 dB"));
        QCOMPARE(dbToLinear(-59.5), 0.0);
        QVERIFY(std::abs(dbToLinear(-6) - 0.501187) < 1e-6);
    }

    // Transition timing: the dissolve borrows the sped-up clip's unused head and ends at the cut;
    // a dip sits across the cut; with no spare media at all, a dissolve uses what there is.
    void transitions() {
        Project p;
        const Uuid v = p.videoTracks[0].id;
        Clip a(v, ClipKind::video, 0, 2);
        Clip b(v, ClipKind::video, 2, 1, 2, 4);
        b.transitionIn = Transition{TransitionKind::dissolve, 0.5, {}};
        p.clips = {a, b};
        auto span = p.transitionSpan(a, b);
        QVERIFY(span && std::abs(span->start - 1.5) < 1e-9 && span->end == 2 && span->fromHead);
        auto plan = p.transitionPlan(b);
        QVERIFY(std::abs(plan.leadIn - 0.5) < 1e-9 && plan.reveal && plan.reveal->start == 1.5);
        p.clips[1].transitionIn = Transition{TransitionKind::dipToBlack, 1, {}};
        auto dip = p.transitionSpan(a, p.clips[1]);
        QVERIFY(dip && dip->start == 1.5 && dip->end == 2.5);
        auto conceal = p.transitionPlan(a).conceal;
        QVERIFY(conceal && conceal->end == 2);
    }

    // Gaps: a gap closes with linked audio following; all gaps close.
    void gaps() {
        Project p;
        const Uuid v = p.videoTracks[0].id, a = p.audioTracks[0].id;
        const Uuid link = newUuid();
        Clip c1(v, ClipKind::solid, 0, 2);
        Clip c2(v, ClipKind::video, 3, 2); c2.linkID = link;
        Clip c2a(a, ClipKind::audio, 3, 2); c2a.linkID = link;
        Clip c3(v, ClipKind::solid, 6, 1);
        p.clips = {c1, c2, c3, c2a};
        auto g = p.gap(v, 2.5);
        QVERIFY(g && g->start == 2 && g->end == 3);
        Project q = p;
        q.pullLeft(v, 3, 1);
        QVERIFY(q.clip(c2.id)->start == 2 && q.clip(c2a.id)->start == 2 && q.clip(c3.id)->start == 5);
        Project r = p;
        r.closeAllGaps(v);
        QVERIFY(r.clip(c2.id)->start == 2 && r.clip(c3.id)->start == 4);
    }

    // In/out: markers survive saving, old files without markers open, markers are snap targets.
    void range() {
        Project p;
        p.inPoint = 1;
        p.outPoint = 2.5;
        Marker m; m.time = 1.2; m.name = QStringLiteral("Beat drop"); m.tint = Marker::Tint::green;
        p.markers = std::vector<Marker>{m};
        const QByteArray bytes = codec::saveBytes(SavedDocument{1, p, {}});
        auto back = codec::decodeDocument(bytes);
        QVERIFY(back.document);
        const Project& bp = back.document->project;
        QVERIFY(bp.markers && bp.markers->front().name == QStringLiteral("Beat drop") && bp.inPoint == 1.0 && bp.outPoint == 2.5);
        QVERIFY(bp == p);
        QJsonObject old = codec::encode(Project());
        old.remove(QStringLiteral("markers"));
        QVERIFY(codec::decodeProject(old));
        const auto snaps = p.snapTargets({});
        QVERIFY(std::find(snaps.begin(), snaps.end(), 1.2) != snaps.end());
        auto pr = p.playRange();
        QVERIFY(pr && pr->start == 1 && pr->end == 2.5);
    }

    void cropped() {
        Project p;
        const Uuid v = p.videoTracks[0].id;
        p.clips = {Clip(v, ClipKind::solid, 0, 4), Clip(v, ClipKind::solid, 4, 4)};
        Marker m; m.time = 5; p.markers = std::vector<Marker>{m};
        p.inPoint = 2; p.outPoint = 6;
        const Project c = p.cropped(*p.playRange());
        QCOMPARE(c.clips.size(), size_t(2));
        QCOMPARE(c.duration(), 4.0);
        QVERIFY(!c.inPoint && !c.outPoint && c.markers->front().time == 3);
    }

    // Relink: relative paths are stamped on save, and found again when the project folder moves.
    void relink() {
        QTemporaryDir work;
        const QString first = work.filePath(QStringLiteral("first")), footage = first + QStringLiteral("/footage");
        QVERIFY(QDir().mkpath(footage));
        Project p;
        for (const char* name : {"a_main.mp4", "b_tone.wav"}) {
            QFile f(footage + u'/' + QLatin1String(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            MediaAsset a = videoAsset();
            a.path = f.fileName();
            a.name = QLatin1String(name);
            p.assets.push_back(a);
        }
        p.stampRelativePaths(first);
        QCOMPARE(*p.assets[0].relativePath, QStringLiteral("footage/a_main.mp4"));
        const QString second = work.filePath(QStringLiteral("second"));
        QVERIFY(QDir().rename(first, second));
        Project moved = p;
        QCOMPARE(moved.resolveMovedMedia(second), 2);
        for (const auto& a : moved.assets) QVERIFY(QFile::exists(a.path));
        // A Mac path, relative to a sibling folder.
        Project mac;
        MediaAsset a = videoAsset();
        a.path = QStringLiteral("/Users/me/Movies/Trip/clip.mov");
        mac.assets = {a};
        mac.stampRelativePaths(QStringLiteral("/Users/me/Projects"));
        QCOMPARE(*mac.assets[0].relativePath, QStringLiteral("../Movies/Trip/clip.mov"));
    }

    void splitAndRipple() {
        Project p;
        const Uuid v = p.videoTracks[0].id;
        const MediaAsset video = videoAsset();
        p.assets = {video};
        Clip c(v, ClipKind::video, 0, 4, 1); c.assetID = video.id; c.fadeIn = 1; c.fadeOut = 1;
        Clip d(v, ClipKind::solid, 4, 2);
        p.clips = {c, d};
        const IdSet right = p.split({c.id}, 2);
        const Clip& l = *p.clip(c.id);
        const Clip& r = *p.clip(*right.begin());
        QVERIFY(l.duration == 2 && l.fadeOut == 0 && l.fadeIn == 1);
        QVERIFY(r.start == 2 && r.sourceStart == 3 && r.fadeIn == 0 && r.fadeOut == 1);
        p.remove({r.id}, true);
        QCOMPARE(p.clip(d.id)->start, 2.0);
        // Overwrite: a clip dropped in the middle of another splits it.
        Project o;
        const Uuid ov = o.videoTracks[0].id;
        Clip under(ov, ClipKind::solid, 0, 10);
        Clip over(ov, ClipKind::solid, 4, 2);
        o.clips = {under, over};
        o.resolveOverlaps({over.id});
        QCOMPARE(o.clips.size(), size_t(3));
        QCOMPARE(o.clip(under.id)->end(), 4.0);
        // Ripple remove across tracks.
        Project rr;
        const Uuid rv = rr.videoTracks[0].id, ra = rr.audioTracks[0].id;
        rr.clips = {Clip(rv, ClipKind::solid, 0, 10), Clip(ra, ClipKind::audio, 0, 10)};
        rr.rippleRemove(2, 5, {rv, ra});
        QCOMPARE(rr.duration(), 7.0);
        QCOMPARE(rr.clips.size(), size_t(4));
    }

    void trimLimits() {
        Project p;
        const Uuid v = p.videoTracks[0].id;
        const MediaAsset video = videoAsset(10);
        p.assets = {video};
        Clip a(v, ClipKind::solid, 0, 2);
        Clip b(v, ClipKind::video, 3, 2, 1); b.assetID = video.id;
        p.clips = {a, b};
        auto [lo, hi] = p.trimLimits(b, true, {});
        QCOMPARE(lo, -1.0); // the head room (1s of footage) and the gap (1s) agree
        QVERIFY(std::abs(hi - (2 - 1.0 / 30)) < 1e-12);
        auto [lo2, hi2] = p.trimLimits(b, false, {});
        QVERIFY(std::abs(lo2 - (1.0 / 30 - 2)) < 1e-12);
        QCOMPARE(hi2, 7.0); // the file's remaining 7s
    }
};

QTEST_GUILESS_MAIN(TestModel)
#include "tst_model.moc"
