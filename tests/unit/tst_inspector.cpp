// The inspector, driven as a person would: picks clips, opens sections, drags the wheels and graphs with the
// mouse and types values, then checks the project and the undo history. With ER_SHOTS=<dir> it also saves a
// picture of each state, for comparing with the Mac by eye.
#include "ui/Controller.h"
#include "ui/InspectorGraphs.h"
#include "ui/InspectorPanel.h"
#include "ui/InspectorWidgets.h"
#include "ui/Theme.h"
#include "media/MediaStore.h"
#include <QApplication>
#include <QFile>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <cmath>

using namespace er;

class TestInspector : public QObject {
    Q_OBJECT
    std::unique_ptr<Controller> c;
    std::unique_ptr<InspectorPanel> panel;
    Uuid video, text;

    EditorModel& m() { return c->model(); }
    const Clip& clip(const Uuid& id) { return *m().project().clip(id); }

    void settle() { for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(); }

    void shot(const char* name) {
        const QString dir = qEnvironmentVariable("ER_SHOTS");
        if (dir.isEmpty()) return;
        settle();
        panel->grab().save(dir + u'/' + QLatin1String(name) + QStringLiteral(".png"));
    }

    template <class T> T* find(const std::function<bool(T*)>& pred = {}) {
        for (T* w : panel->findChildren<T*>()) if (w->isVisible() && (!pred || pred(w))) return w;
        return nullptr;
    }
    InspectorSection* section(const QString& title) {
        for (auto* s : panel->findChildren<InspectorSection*>()) {
            // The header paints the title; find it through the saved key it uses.
            if (s->property("title").toString() == title) return s;
        }
        return nullptr;
    }

    static void send(QWidget* w, QEvent::Type t, QPointF at, Qt::MouseButtons buttons) {
        QMouseEvent e(t, at, w->mapToGlobal(at), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(w, &e);
    }
    /// A press, a few moves with the button held, and a release.
    static void drag(QWidget* w, QPointF from, QPointF to) {
        send(w, QEvent::MouseButtonPress, from, Qt::LeftButton);
        for (int i = 1; i <= 6; ++i) send(w, QEvent::MouseMove, from + (to - from) * (i / 6.0), Qt::LeftButton);
        send(w, QEvent::MouseButtonRelease, to, Qt::NoButton);
    }
    static void click(QWidget* w, QPointF at) {
        send(w, QEvent::MouseButtonPress, at, Qt::LeftButton);
        send(w, QEvent::MouseButtonRelease, at, Qt::NoButton);
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        const QString path = QStringLiteral(ER_TEST_MEDIA "/a_main.mp4");
        if (!QFile::exists(path)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Theme::loadFonts();
        qApp->setStyleSheet(Theme::styleSheet());
        c = std::make_unique<Controller>();
        auto asset = c->media().importFile(path);
        QVERIFY(asset);
        m().changeLibrary([&](Project& p) { p.assets.push_back(*asset); });
        const Project& p = m().project();
        Clip v(p.videoTracks[0].id, ClipKind::video, 0, 4);
        v.assetID = asset->id;
        Clip t(p.videoTracks[1].id, ClipKind::text, 0, 3);
        t.text.string = QStringLiteral("Hello");
        video = v.id;
        text = t.id;
        m().mutate([&](Project& q) { q.clips.push_back(v); q.clips.push_back(t); });
        panel = std::make_unique<InspectorPanel>(*c);
        panel->resize(InspectorPanel::width, 1400);
        panel->show();
        settle();
    }

    void sequence() {
        m().setSelection({});
        settle();
        QVERIFY(section("Frame") && section("Frame rate"));
        shot("inspector-sequence");
    }

    void gradeWheels() {
        m().setSelection({video});
        settle();
        auto* grade = section("Grade");
        QVERIFY(grade);
        // Folded by default; open it as a person would, by clicking its header.
        QVERIFY(grade->collapsed());
        click(grade->findChild<QWidget*>(QStringLiteral("sectionHeader")), QPointF(20, 8));
        QVERIFY(!grade->collapsed());
        QVERIFY(QSettings().value("inspector.collapsed.Grade").isValid());
        settle();
        auto* shadows = find<ColorWheel>();
        QVERIFY(shadows);
        const size_t before = m().undoDepth();
        const QPointF centre(shadows->width() / 2.0, ColorWheel::size / 2.0);
        drag(shadows, centre, centre + QPointF(20, 0));
        settle();
        QVERIFY(clip(video).grade);
        QVERIFY(clip(video).grade->shadows.x > 0.5);
        QCOMPARE(clip(video).grade->shadows.y, 0.0);
        QCOMPARE(m().undoDepth(), before + 1);
        shot("inspector-grade");
        // Double-click puts it back to neutral (and the grade goes when nothing else is set). The section was
        // rebuilt when the grade changed (its Reset link appeared), so the wheel is found again.
        shadows = find<ColorWheel>();
        QVERIFY(shadows);
        QTest::mouseDClick(shadows, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
        settle();
        QVERIFY(!clip(video).grade);
    }

    void typedValue() {
        m().setSelection({video});
        settle();
        auto* fade = section("Fade");
        QVERIFY(fade);
        if (fade->collapsed()) click(fade->findChild<QWidget*>(QStringLiteral("sectionHeader")), QPointF(20, 8));
        settle();
        // The first number field in the Fade section is Fade In: type 1.5 and leave it.
        NumberField* field = nullptr;
        for (auto* f : fade->findChildren<NumberField*>()) if (f->isVisible()) { field = f; break; }
        QVERIFY(field);
        field->setFocus();
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("1.5s"));
        QTest::keyClick(field, Qt::Key_Return);
        settle();
        QVERIFY2(std::abs(clip(video).fadeIn - 1.5) < 1e-9, qPrintable(QString::number(clip(video).fadeIn)));
        // ↑ steps it to the next tenth. (The section was rebuilt: it's marked now that there's a fade.)
        field = nullptr;
        for (auto* f : section("Fade")->findChildren<NumberField*>()) if (f->isVisible()) { field = f; break; }
        QVERIFY(field);
        field->setFocus();
        QTest::keyClick(field, Qt::Key_Up);
        settle();
        QVERIFY(std::abs(clip(video).fadeIn - 1.6) < 0.02);
        field->clearFocus();
    }

    void speedRamp() {
        m().setSelection({video});
        settle();
        RampButton* ramp = nullptr;
        for (auto* b : panel->findChildren<RampButton*>()) if (b->isVisible() && b->toolTip().contains("Speed ramp")) ramp = b;
        QVERIFY(ramp);
        ramp->click();
        settle();
        QVERIFY(clip(video).isRamped());
        auto* graph = find<SpeedRampGraph>();
        QVERIFY(graph);
        shot("inspector-ramp");
        // The In shape (the default): its right dot sets where the ease ends and the speed it eases to. Pull it
        // up toward 200%.
        const double h = SpeedRampGraph::graphHeight, w = graph->width() - 16;
        auto y = [h](double v) { return 6 + (h - 12) * (1 - (std::log(v) - std::log(0.1)) / (std::log(10) - std::log(0.1))); };
        const QPointF dot(8 + 0.6 * w, 8 + y(0.5));
        const size_t before = m().undoDepth();
        drag(graph, dot, QPointF(8 + 0.6 * w, 8 + y(2)));
        settle();
        QVERIFY2(clip(video).speed > 1.5 && clip(video).speed < 2.5, qPrintable(QString::number(clip(video).speed)));
        QCOMPARE(m().undoDepth(), before + 1);
        QVERIFY(!m().showPlayheadTime());
        // Off again: the ramp goes, the speed stays.
        for (auto* b : panel->findChildren<RampButton*>()) if (b->isVisible() && b->toolTip().contains("Remove the speed ramp")) ramp = b;
        ramp->click();
        settle();
        QVERIFY(!clip(video).isRamped());
        QVERIFY(!find<SpeedRampGraph>());
        m().undo();
        m().undo();
        m().undo();
        settle();
    }

    void zoom() {
        m().setSelection({video});
        settle();
        auto* tr = section("Transform");
        QVERIFY(tr);
        if (tr->collapsed()) click(tr->findChild<QWidget*>(QStringLiteral("sectionHeader")), QPointF(20, 8));
        settle();
        RampButton* zoom = nullptr;
        for (auto* b : panel->findChildren<RampButton*>()) if (b->isVisible() && b->toolTip().startsWith("Zoom:")) zoom = b;
        QVERIFY(zoom);
        zoom->click();
        settle();
        QVERIFY(clip(video).transform && clip(video).transform->zoom);
        auto* graph = find<ZoomGraph>();
        QVERIFY(graph);
        shot("inspector-zoom");
        // The top line sets the Scale: drag it down toward the full frame.
        const ZoomMove z = *clip(video).transform->zoom;
        const double w = graph->width() - 16, len = clip(video).duration;
        const double midX = 8 + ((z.p2 / len) * w + w - 5) / 2;
        const double top = 3; // the graph's top while at 200%
        const double h = ZoomGraph::graphHeight;
        auto y = [&](double v) { return 6 + (h - 12) * (1 - std::log(v) / std::log(top)); };
        drag(graph, QPointF(midX, 8 + y(2)), QPointF(midX, 8 + y(1.5)));
        settle();
        const double s = clip(video).transform->scale;
        QVERIFY2(s > 1.3 && s < 1.7, qPrintable(QString::number(s)));
    }

    void textClip() {
        m().setSelection({text});
        settle();
        QVERIFY(section("Text"));
        auto* edit = find<QPlainTextEdit>([](QPlainTextEdit* e) { return !qobject_cast<CaptionTextField*>(e); });
        QVERIFY(edit);
        edit->setFocus();
        edit->moveCursor(QTextCursor::End);
        QTest::keyClicks(edit, QStringLiteral(" there"));
        settle();
        QCOMPARE(clip(text).text.string, QStringLiteral("Hello there"));
        // Still the same field (it isn't rebuilt under the typing).
        QVERIFY(edit->hasFocus());
        shot("inspector-text");
    }
};

QTEST_MAIN(TestInspector)
#include "tst_inspector.moc"
