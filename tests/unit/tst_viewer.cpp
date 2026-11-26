// The viewer and its transport: dragging the picked picture moves its framing (and snaps back to centre),
// a title moves by its position, typing a time goes there, the zoom, and the full-screen viewer.
#include "ui/Controller.h"
#include "ui/PreviewPanel.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using namespace er;

class TestViewer : public QObject {
    Q_OBJECT
    std::unique_ptr<Controller> c;
    std::unique_ptr<PreviewPanel> panel;
    Uuid video, title;
    EditorModel& m() { return c->model(); }
    void settle() { for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(); }
    static void send(QWidget* w, QEvent::Type t, QPointF at, Qt::MouseButtons buttons) {
        QMouseEvent e(t, at, w->mapToGlobal(at), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(w, &e);
    }
    static void drag(QWidget* w, QPointF from, QPointF to) {
        send(w, QEvent::MouseButtonPress, from, Qt::LeftButton);
        for (int i = 1; i <= 6; ++i) send(w, QEvent::MouseMove, from + (to - from) * (i / 6.0), Qt::LeftButton);
        send(w, QEvent::MouseButtonRelease, to, Qt::NoButton);
    }
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        const QString path = QStringLiteral(ER_TEST_MEDIA "/a_main.mp4");
        if (!QFile::exists(path)) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        Theme::loadFonts();
        c = std::make_unique<Controller>();
        auto a = c->media().importFile(path);
        m().changeLibrary([&](Project& p) { p.assets.push_back(*a); });
        Clip v(m().project().videoTracks[0].id, ClipKind::video, 0, 4);
        v.assetID = a->id;
        Clip t(m().project().videoTracks[1].id, ClipKind::text, 0, 4);
        video = v.id;
        title = t.id;
        m().mutate([&](Project& p) { p.clips = {v, t}; });
        panel = std::make_unique<PreviewPanel>(*c);
        panel->resize(900, 600);
        panel->show();
        settle();
    }

    void dragFraming() {
        m().setSelection({video});
        Viewer* v = panel->viewer();
        const QRectF pic = v->picture();
        // A quarter of the picture to the right; a nudge of 3 px down snaps back to centre.
        drag(v, pic.center(), pic.center() + QPointF(pic.width() / 4, 3));
        settle();
        const Clip* c0 = m().project().clip(video);
        QVERIFY(c0->transform);
        QVERIFY2(std::abs(c0->transform->x - 0.25) < 0.01, qPrintable(QString::number(c0->transform->x)));
        QCOMPARE(c0->transform->y, 0.0);
    }

    void dragTitle() {
        m().setSelection({title});
        Viewer* v = panel->viewer();
        const QRectF pic = v->picture();
        drag(v, pic.center(), pic.center() + QPointF(0, pic.height() / 4));
        settle();
        QVERIFY(std::abs(m().project().clip(title)->text.y - 0.75) < 0.01);
    }

    void typeTime() {
        QWidget* field = nullptr;
        for (auto* w : panel->findChildren<QWidget*>()) if (w->toolTip() == QStringLiteral("Click to type a time")) field = w;
        QVERIFY(field);
        QTest::mouseClick(field, Qt::LeftButton);
        auto* edit = field->findChild<QLineEdit*>();
        QVERIFY(edit && edit->isVisible());
        edit->setText(QStringLiteral("00:00:02:15"));
        QTest::keyClick(edit, Qt::Key_Return);
        settle();
        QVERIFY(std::abs(m().playhead() - 2.5) < 0.02);
    }

    void zoomAndFullScreen() {
        panel->setZoom(2.0);
        settle();
        QVERIFY(panel->viewer()->width() > 900);
        panel->setZoom(std::nullopt);
        settle();
        QVERIFY(panel->viewer()->width() < 900);
        QSignalSpy spy(&m(), &EditorModel::viewerOnlyChanged);
        m().toggleViewerFullScreen();
        QVERIFY(m().viewerOnly());
        m().toggleViewerFullScreen();
        QCOMPARE(spy.count(), 2);
    }
};

QTEST_MAIN(TestViewer)
#include "tst_viewer.moc"
