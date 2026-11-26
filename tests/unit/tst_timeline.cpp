// The timeline, driven with the mouse: selecting, moving (and Alt-copying), trimming, fades, the gain line,
// boxing clips in, scrubbing, markers and the in/out range, the cut tool, the wheel, the range bars and drops
// from the Media panel; each checked against the project and the undo history. With ER_SHOTS=<dir> it saves
// pictures too.
#include "ui/Controller.h"
#include "ui/MarkerPanel.h"
#include "ui/Theme.h"
#include "ui/TimelinePanel.h"
#include <QApplication>
#include <QFile>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <cmath>

using namespace er;

class TestTimeline : public QObject {
    Q_OBJECT
    std::unique_ptr<Controller> c;
    std::unique_ptr<TimelinePanel> panel;
    MediaAsset video, tone;
    Uuid v1, a1, music;

    EditorModel& m() { return c->model(); }
    TimelineCanvas* canvas() { return panel->canvas(); }
    const Clip& clip(const Uuid& id) { return *m().project().clip(id); }
    void settle() { for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(); }

    void shot(const char* name) {
        const QString dir = qEnvironmentVariable("ER_SHOTS");
        if (dir.isEmpty()) return;
        settle();
        panel->grab().save(dir + u'/' + QLatin1String(name) + QStringLiteral(".png"));
    }

    /// Where a clip is on the canvas.
    QRectF rect(const Uuid& id) {
        const Clip& cl = clip(id);
        const TimelineLayout layout(m().project(), m().view().rowHeight);
        auto band = layout.band(cl.trackID);
        return QRectF(canvas()->x(cl.start), TimelineLayout::ruler + band->first - m().view().scrollY, cl.duration * m().view().pps, band->second);
    }

    static void send(QWidget* w, QEvent::Type t, QPointF at, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent e(t, at, w->mapToGlobal(at), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
        QApplication::sendEvent(w, &e);
    }
    static void drag(QWidget* w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        send(w, QEvent::MouseButtonPress, from, Qt::LeftButton, mods);
        for (int i = 1; i <= 8; ++i) send(w, QEvent::MouseMove, from + (to - from) * (i / 8.0), Qt::LeftButton, mods);
        send(w, QEvent::MouseButtonRelease, to, Qt::NoButton, mods);
    }
    static void click(QWidget* w, QPointF at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        send(w, QEvent::MouseButtonPress, at, Qt::LeftButton, mods);
        send(w, QEvent::MouseButtonRelease, at, Qt::NoButton, mods);
    }

    void reset() {
        while (m().canUndo()) m().undo();
        m().setSelection({});
        m().setView(80.0, 0.0);
        settle();
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        const QString a = QStringLiteral(ER_TEST_MEDIA "/a_main.mp4"), b = QStringLiteral(ER_TEST_MEDIA "/b_tone.wav");
        if (!QFile::exists(a) || !QFile::exists(b)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Theme::loadFonts();
        qApp->setStyleSheet(Theme::styleSheet());
        c = std::make_unique<Controller>();
        video = *c->media().importFile(a);
        tone = *c->media().importFile(b);
        m().changeLibrary([&](Project& p) { p.assets = {video, tone}; });
        auto clips = m().makeClips(DragPayload::ofAsset(video.id), 0, m().project().videoTracks[0].id, m().project().audioTracks[0].id);
        QCOMPARE(clips.size(), size_t(2));
        v1 = clips[0].id;
        a1 = clips[1].id;
        auto song = m().makeClips(DragPayload::ofAsset(tone.id), 6, m().project().videoTracks[0].id, m().project().audioTracks[1].id);
        QCOMPARE(song.size(), size_t(1));
        music = song[0].id;
        clips.push_back(song[0]);
        m().changeLibrary([&](Project& p) { p.clips = clips; }, true);
        panel = std::make_unique<TimelinePanel>(*c);
        panel->resize(1200, 420);
        panel->show();
        settle();
        m().setView(80.0, 0.0);
        settle();
    }

    void opens() {
        // Rows as tall as fit, no taller than 62, with the tracks centred.
        QVERIFY(m().view().rowHeight <= 62 && m().view().rowHeight >= TimelineViewState::rowMin);
        QVERIFY(m().view().viewWidth > 900);
        // Filmstrip frames and waveforms are made in the background: wait for them before the picture.
        c->sound().warm(m().project().assets);
        for (int i = 0; i < 60; ++i) { canvas()->repaint(); QTest::qWait(50); }
        shot("timeline-open");
        if (const QString dir = qEnvironmentVariable("ER_SHOTS"); !dir.isEmpty()) m().writeTo(dir + QStringLiteral("/timeline.room"));
    }

    void selectAndMove() {
        reset();
        const QRectF r = rect(v1);
        click(canvas(), r.center());
        settle();
        // A linked clip picks its sound too.
        QVERIFY(m().selection().count(v1) && m().selection().count(a1));
        const size_t before = m().undoDepth();
        const double pps = m().view().pps;
        drag(canvas(), r.center(), r.center() + QPointF(pps * 1.0, 0));
        settle();
        QVERIFY2(std::abs(clip(v1).start - 1.0) < 0.05, qPrintable(QString::number(clip(v1).start)));
        QCOMPARE(clip(a1).start, clip(v1).start);
        QCOMPARE(m().undoDepth(), before + 1);
        // Shift-click adds the music.
        click(canvas(), rect(music).center(), Qt::ShiftModifier);
        QVERIFY(m().selection().count(music) && m().selection().count(v1));
    }

    void altCopy() {
        reset();
        const size_t clipsBefore = m().project().clips.size();
        const QRectF r = rect(music);
        const double was = clip(music).start;
        click(canvas(), r.center());
        // Alt held: the original stays and a copy lands where it's dropped.
        drag(canvas(), r.center(), r.center() + QPointF(m().view().pps * 3, 0), Qt::AltModifier);
        settle();
        QCOMPARE(m().project().clips.size(), clipsBefore + 1);
        QCOMPARE(clip(music).start, was);
        bool copyThere = false;
        for (const auto& c : m().project().clips) if (c.id != music && c.assetID == tone.id && std::abs(c.start - (was + 3)) < 0.05) copyThere = true;
        QVERIFY(copyThere);
    }

    void trim() {
        reset();
        const QRectF r = rect(v1);
        const double before = clip(v1).duration;
        drag(canvas(), QPointF(r.right() - 2, r.center().y()), QPointF(r.right() - 2 - m().view().pps * 1.0, r.center().y()));
        settle();
        QVERIFY2(std::abs(clip(v1).duration - (before - 1.0)) < 0.05, qPrintable(QString::number(clip(v1).duration)));
        // Its sound is trimmed with it.
        QCOMPARE(clip(a1).duration, clip(v1).duration);
        QVERIFY(!c->sourcePreview().current());
    }

    void fadeDot() {
        reset();
        m().mutate([&](Project& p) { if (auto i = p.index(v1)) p.clips[*i].fadeIn = 0.5; });
        settle();
        const QRectF r = rect(v1).adjusted(0, 2, 0, -2);
        const QPointF dot(r.left() + 0.5 * m().view().pps, r.top() + 8);
        drag(canvas(), dot, dot + QPointF(m().view().pps * 0.5, 0));
        settle();
        QVERIFY2(std::abs(clip(v1).fadeIn - 1.0) < 0.05, qPrintable(QString::number(clip(v1).fadeIn)));
        shot("timeline-fade");
    }

    void gainLine() {
        reset();
        const QRectF r = rect(music).adjusted(0, 2, 0, -2);
        // 0 dB sits 30% down the clip.
        const QPointF line(r.center().x(), r.top() + 0.3 * r.height());
        drag(canvas(), line, line + QPointF(0, r.height() * 0.2));
        settle();
        QVERIFY2(clip(music).gainDB < -5, qPrintable(QString::number(clip(music).gainDB)));
        shot("timeline-gain");
        // Double-click on the line puts it back to 0 dB.
        const double y = r.top() + (0.3 + -clip(music).gainDB / 60 * 0.62) * r.height();
        QTest::mouseDClick(canvas(), Qt::LeftButton, Qt::NoModifier, QPointF(r.center().x(), y).toPoint());
        settle();
        QCOMPARE(clip(music).gainDB, 0.0);
    }

    void marquee() {
        reset();
        // From empty space after everything, boxing back over the music.
        const QRectF r = rect(music);
        drag(canvas(), QPointF(r.right() + 40, r.top() - 4), QPointF(r.center().x(), r.bottom() - 4));
        settle();
        QVERIFY(m().selection().count(music));
        QVERIFY(!m().selection().count(v1));
    }

    void scrubAndMarkers() {
        reset();
        const double pps = m().view().pps;
        drag(canvas(), QPointF(canvas()->x(1), 12), QPointF(canvas()->x(2.5), 12));
        settle();
        QVERIFY(std::abs(m().playhead() - 2.5) < 0.1);
        // A marker: dragged along the ruler, then clicked to open its panel.
        m().seek(1.0);
        m().addMarker();
        settle();
        QCOMPARE(m().project().markerList().size(), size_t(1));
        m().seek(5.0);
        settle();
        const double mx = canvas()->x(1.0);
        drag(canvas(), QPointF(mx + 3, 8), QPointF(mx + 3 + pps * 1.0, 8));
        settle();
        QVERIFY2(std::abs(m().project().markerList()[0].time - 2.0) < 0.1, qPrintable(QString::number(m().project().markerList()[0].time)));
        click(canvas(), QPointF(canvas()->x(m().project().markerList()[0].time) + 3, 8));
        settle();
        QVERIFY(m().editingMarker.has_value());
        QVERIFY(canvas()->findChild<MarkerPanel*>());
        shot("timeline-marker");
        m().setEditingMarker(std::nullopt);
        settle();
        // The in/out range: drag its end.
        m().seek(1);
        m().setInPoint();
        m().seek(3);
        m().setOutPoint();
        // (Near the playhead a click takes the playhead, as on the Mac: move it away from the edge first.)
        m().seek(0.2);
        settle();
        drag(canvas(), QPointF(canvas()->x(3), 14), QPointF(canvas()->x(3) + pps, 14));
        settle();
        QVERIFY(m().project().outPoint && std::abs(*m().project().outPoint - 4) < 0.1);
        shot("timeline-range");
    }

    void cutTool() {
        reset();
        m().tool = Tool::blade;
        panel->refreshToggles();
        const size_t before = m().project().clips.size();
        const QRectF r = rect(music);
        QTest::mouseMove(canvas(), QPointF(r.left() + m().view().pps * 1.0, r.center().y()).toPoint());
        click(canvas(), QPointF(r.left() + m().view().pps * 1.0, r.center().y()));
        settle();
        QCOMPARE(m().project().clips.size(), before + 1);
        m().tool = Tool::move;
        panel->refreshToggles();
    }

    void wheelAndBars() {
        reset();
        const double pps = m().view().pps;
        QWheelEvent zoom(QPointF(300, 200), canvas()->mapToGlobal(QPointF(300, 200)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                         Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas(), &zoom);
        QVERIFY(m().view().pps > pps);
        // The wheel scrolls sideways.
        m().setView(400.0, 0.0);
        QWheelEvent scroll(QPointF(300, 200), canvas()->mapToGlobal(QPointF(300, 200)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                           Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas(), &scroll);
        QVERIFY(m().view().scrollX > 0);
        // The bar under the timeline: drag its thumb.
        RangeBar* bar = nullptr;
        for (auto* b : panel->findChildren<RangeBar*>()) if (b->height() == int(RangeBarStyle::thickness) && b->width() > 100) bar = b;
        QVERIFY(bar);
        const double sx = m().view().scrollX;
        const double thumbX = sx / m().view().pps / m().extent() * bar->width();
        drag(bar, QPointF(thumbX + 30, 7), QPointF(thumbX + 80, 7));
        QVERIFY(m().view().scrollX > sx);
        m().zoomToFit();
        shot("timeline-zoomed");
    }

    void audioScrub() {
        reset();
        // Turning it on makes the scrub mix: the edit's sound, held in memory.
        c->sound().warm(m().project().assets);
        m().setAudioScrub(true);
        c->scrubber().waitReady(10000);
        QVERIFY(c->scrubber().ready());
        // The tone is heard where it sits (6 s on), silence where nothing plays (5 s).
        QVERIFY2(c->scrubber().peak(7.0) > 0.05, qPrintable(QString::number(c->scrubber().peak(7.0))));
        QVERIFY(c->scrubber().peak(5.0) < 0.001);
        m().setAudioScrub(false);
    }

    void drop() {
        reset();
        const size_t before = m().project().clips.size();
        QMimeData mime;
        mime.setData(QStringLiteral("application/x-editroom-asset"), (QStringLiteral("editroom:") + tone.id.toString()).toUtf8());
        const TimelineLayout layout(m().project(), m().view().rowHeight);
        double y = 0;
        for (const auto& row : layout.rows) if (row.track.kind == TrackKind::audio && row.index == 0) y = TimelineLayout::ruler + row.y - m().view().scrollY + row.height / 2;
        const QPointF at(canvas()->x(9), y);
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas(), &enter);
        QDragMoveEvent move(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas(), &move);
        shot("timeline-drop");
        QDropEvent dropped(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas(), &dropped);
        settle();
        QCOMPARE(m().project().clips.size(), before + 1);
    }
};

QTEST_MAIN(TestTimeline)
#include "tst_timeline.moc"
