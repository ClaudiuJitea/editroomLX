// The Media panel: a folder brought in whole stays linked (its subfolders show as folders), opening folders and
// going back, searching, picking with Shift and Ctrl, hover-scrubbing into the viewer, a file added to the
// folder on disk coming in, a moved file found again with Locate's relink, and deleting a folder.
#include "ui/Controller.h"
#include "ui/LibraryPanel.h"
#include "ui/Theme.h"
#include "model/MediaFolders.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace er;

class TestLibrary : public QObject {
    Q_OBJECT
    std::unique_ptr<Controller> c;
    std::unique_ptr<LibraryPanel> panel;
    QTemporaryDir tmp;
    QString root;

    EditorModel& m() { return c->model(); }
    MediaGrid* grid() { return panel->findChild<MediaGrid*>(); }
    void settle(int ms = 0) { QTest::qWait(ms); for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(); }
    bool waitImports(int ms = 10000) {
        for (int i = 0; i < ms / 20; ++i) { settle(20); if (c->importCount() == 0) return true; }
        return false;
    }
    void shot(const char* name) {
        const QString dir = qEnvironmentVariable("ER_SHOTS");
        if (dir.isEmpty()) return;
        for (int i = 0; i < 30; ++i) settle(30);
        panel->grab().save(dir + u'/' + QLatin1String(name) + QStringLiteral(".png"));
    }
    std::optional<size_t> itemNamed(const QString& name) {
        const auto& items = grid()->items();
        for (size_t i = 0; i < items.size(); ++i)
            if ((items[i].folder ? MediaTree::name(items[i].folderPath) : items[i].asset.name) == name) return i;
        return std::nullopt;
    }
    QPoint centre(size_t i) { return grid()->items()[i].rect.center().toPoint(); }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        const QString media = QStringLiteral(ER_TEST_MEDIA);
        if (!QFile::exists(media + QStringLiteral("/a_main.mp4"))) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        root = tmp.path() + QStringLiteral("/Shoot");
        QDir().mkpath(root + QStringLiteral("/Drone"));
        QFile::copy(media + QStringLiteral("/a_main.mp4"), root + QStringLiteral("/main.mp4"));
        QFile::copy(media + QStringLiteral("/b_tone.wav"), root + QStringLiteral("/tone.wav"));
        QFile::copy(media + QStringLiteral("/sdr_h264.mp4"), root + QStringLiteral("/Drone/flight.mp4"));
        Theme::loadFonts();
        qApp->setStyleSheet(Theme::styleSheet());
        c = std::make_unique<Controller>();
        panel = std::make_unique<LibraryPanel>(*c);
        panel->resize(280, 600);
        panel->show();
        settle();
    }

    void importFolder() {
        c->importFiles({root}, false);
        QVERIFY(waitImports());
        QCOMPARE(m().project().assets.size(), size_t(3));
        QVERIFY(m().project().folders && m().project().folders->size() == 1);
        // At the top: the linked folder.
        QVERIFY(itemNamed(QStringLiteral("Shoot")));
        shot("library-top");
    }

    void openAndBack() {
        auto shoot = itemNamed(QStringLiteral("Shoot"));
        QVERIFY(shoot);
        QTest::mouseDClick(grid(), Qt::LeftButton, Qt::NoModifier, centre(*shoot));
        settle();
        QCOMPARE(*m().mediaFolder, root);
        // Inside: its subfolder first, then its own files.
        QVERIFY(itemNamed(QStringLiteral("Drone")));
        QVERIFY(itemNamed(QStringLiteral("main")));
        QVERIFY(!itemNamed(QStringLiteral("flight")));
        QCOMPARE(grid()->items().front().folder, true);
        shot("library-folder");
        // List layout too.
        setMediaLayout(MediaLayout::list);
        emit m().mediaSelectionChanged();
        settle();
        shot("library-list");
        setMediaLayout(MediaLayout::thumbnails);
        emit m().mediaSelectionChanged();
        // Back: the title's chevron.
        auto* title = panel->findChild<QWidget*>(QStringLiteral("mediaTitle"));
        QTest::mouseClick(title, Qt::LeftButton, Qt::NoModifier, QPoint(8, 10));
        settle();
        QVERIFY(!m().mediaFolder);
    }

    void search() {
        auto* field = panel->findChild<QLineEdit*>(QStringLiteral("mediaSearch"));
        // Searching from the top reaches into folders, and says where each find is.
        field->setText(QStringLiteral("flight"));
        settle();
        QCOMPARE(grid()->items().size(), size_t(1));
        QVERIFY(grid()->items().front().place.has_value());
        field->setText(QStringLiteral("\"hello\""));
        settle();
        // No transcripts: nothing says it.
        QVERIFY(grid()->items().empty());
        field->clear();
        settle();
    }

    void picking() {
        m().openMediaFolder(root);
        settle();
        const auto a = itemNamed(QStringLiteral("main")), b = itemNamed(QStringLiteral("tone")), d = itemNamed(QStringLiteral("Drone"));
        QVERIFY(a && b && d);
        QTest::mouseClick(grid(), Qt::LeftButton, Qt::NoModifier, centre(*d));
        QTest::mouseClick(grid(), Qt::LeftButton, Qt::ShiftModifier, centre(*b));
        QCOMPARE(m().folderSelection.size(), size_t(1));
        QCOMPARE(m().mediaSelection.size(), size_t(2));
        QTest::mouseClick(grid(), Qt::LeftButton, Qt::ControlModifier, centre(*a));
        QCOMPARE(m().mediaSelection.size(), size_t(1));
        QTest::mouseClick(grid(), Qt::LeftButton, Qt::NoModifier, centre(*a));
        QCOMPARE(m().mediaSelection.size(), size_t(1));
        QVERIFY(m().folderSelection.empty());
    }

    void hoverScrub() {
        const auto a = itemNamed(QStringLiteral("main"));
        QVERIFY(a);
        const QRectF pic = grid()->items()[*a].picture;
        QTest::mouseMove(grid(), QPointF(pic.left() + pic.width() * 0.75, pic.center().y()).toPoint());
        settle();
        QVERIFY(c->sourcePreview().current());
        QVERIFY(std::abs(c->sourcePreview().current()->time - 3.0) < 0.2);
        QTest::mouseMove(grid(), QPoint(2, int(grid()->height()) - 2));
        settle();
        QVERIFY(!c->sourcePreview().current());
    }

    void newFileComesIn() {
        QFile::copy(QStringLiteral(ER_TEST_MEDIA "/photo.jpg"), root + QStringLiteral("/still.jpg"));
        bool done = false;
        c->syncFolders([&] { done = true; });
        for (int i = 0; i < 500 && !done; ++i) settle(20);
        QVERIFY(waitImports());
        QCOMPARE(m().project().assets.size(), size_t(4));
    }

    void relinkMoved() {
        // The tone moved: Locate's relink points it there, and the clip keeps working.
        const QString moved = tmp.path() + QStringLiteral("/elsewhere");
        QDir().mkpath(moved);
        QVERIFY(QFile::rename(root + QStringLiteral("/tone.wav"), moved + QStringLiteral("/tone.wav")));
        Uuid id;
        for (const auto& a : m().project().assets) if (a.name == QStringLiteral("tone")) id = a.id;
        QVERIFY(c->relink(id, moved + QStringLiteral("/tone.wav")));
        QCOMPARE(m().project().asset(id)->path, moved + QStringLiteral("/tone.wav"));
    }

    void deleteFolder() {
        m().openMediaFolder(std::nullopt);
        settle();
        m().removeFolder(root);
        settle();
        // Everything in it goes (the files stay on disk); the moved tone isn't in it any more.
        QVERIFY(!m().project().folders || m().project().folders->empty());
        QCOMPARE(m().project().assets.size(), size_t(1));
        QVERIFY(QFile::exists(root + QStringLiteral("/main.mp4")));
    }
};

QTEST_MAIN(TestLibrary)
#include "tst_library.moc"
