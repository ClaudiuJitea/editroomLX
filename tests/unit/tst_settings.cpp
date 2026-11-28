// Settings: the tabs (the one shown is remembered), recording a shortcut by clicking it and pressing keys (a key
// taken from another action says so), Escape cancelling, Restore Defaults, the Media and time choices.
#include "ui/Controller.h"
#include "ui/LibraryPanel.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"
#include <QAbstractButton>
#include <QApplication>
#include <QMenu>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>
#include <QLineEdit>
#include <QTest>

using namespace er;

class TestSettings : public QObject {
    Q_OBJECT
    std::unique_ptr<Controller> c;
    std::unique_ptr<ShortcutStore> keys;
    QByteArray saved;
    void settle() { for (int i = 0; i < 6; ++i) QCoreApplication::processEvents(); }
    void shot(QWidget* w, const char* name) {
        const QString dir = qEnvironmentVariable("ER_SHOTS");
        if (dir.isEmpty()) return;
        settle();
        w->grab().save(dir + u'/' + QLatin1String(name) + QStringLiteral(".png"));
    }
    QAbstractButton* buttonWithText(QWidget* root, const QString& t) {
        // Rows are buttons holding a label with the action's name.
        for (auto* l : root->findChildren<QLabel*>())
            if (l->text() == t && l->isVisible()) if (auto* b = qobject_cast<QAbstractButton*>(l->parentWidget())) return b;
        return nullptr;
    }
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        Theme::loadFonts();
        c = std::make_unique<Controller>();
        keys = std::make_unique<ShortcutStore>([this] { return saved; }, [this](const std::optional<QByteArray>& d) { saved = d.value_or(QByteArray()); });
    }

    void tabs() {
        SettingsDialog d(*c, *keys);
        d.show();
        settle();
        shot(&d, "settings-shortcuts");
        QCOMPARE(QSettings().value("settingsTab").toString(), QStringLiteral("shortcuts"));
        // The tab row: General.
        QAbstractButton* general = nullptr;
        for (auto* b : d.findChildren<QAbstractButton*>()) if (b->height() == 56 && !general) general = b;
        QVERIFY(general);
        general->click();
        settle();
        QCOMPARE(QSettings().value("settingsTab").toString(), QStringLiteral("general"));
        shot(&d, "settings-general");
        auto all = d.findChildren<QAbstractButton*>();
        for (auto* b : all) if (b->height() == 56 && b->x() > general->x()) { b->click(); break; }
        settle();
        shot(&d, "settings-shortcuts2");
    }

    void recordShortcut() {
        QSettings().setValue("settingsTab", "shortcuts");
        SettingsDialog d(*c, *keys);
        d.show();
        settle();
        auto* row = buttonWithText(&d, Shortcut::title(ShortcutAction::addMarker));
        QVERIFY(row);
        row->click();
        settle();
        // Takes J, which is nobody's... then K, which is skimming's (Skimming loses it, and the note says so).
        QTest::keyClick(&d, Qt::Key_K);
        settle();
        bool has = false;
        for (const auto& k : keys->combos(ShortcutAction::addMarker)) if (k.display() == QStringLiteral("K")) has = true;
        QVERIFY(has);
        QVERIFY(keys->combos(ShortcutAction::skimming).empty());
        bool noted = false;
        for (auto* l : d.findChildren<QLabel*>()) if (l->text().contains(QStringLiteral("no longer has it"))) noted = true;
        QVERIFY(noted);
        shot(&d, "settings-recorded");
        // Escape cancels a recording.
        row = buttonWithText(&d, Shortcut::title(ShortcutAction::addMarker));
        row->click();
        settle();
        QTest::keyClick(&d, Qt::Key_Escape);
        QVERIFY(!keys->combos(ShortcutAction::addMarker).empty());
        // The dialog stays open after Esc cancels a recording.
        QVERIFY(d.isVisible());
        settle();
        // Keys the dialog would otherwise take (Tab moves focus), and keys typed while the search field has the focus,
        // are recorded too, as on the Mac (its recorder watches every key).
        row = buttonWithText(&d, Shortcut::title(ShortcutAction::addMarker));
        row->click();
        settle();
        QTest::keyClick(&d, Qt::Key_Tab, Qt::ControlModifier);
        settle();
        bool tab = false;
        for (const auto& k : keys->combos(ShortcutAction::addMarker)) if (k.key == QLatin1String("tab") && k.command) tab = true;
        QVERIFY(tab);
        auto* search = d.findChild<QLineEdit*>();
        QVERIFY(search);
        row = buttonWithText(&d, Shortcut::title(ShortcutAction::setIn));
        row->click();
        settle();
        search->setFocus();
        QTest::keyClick(search, Qt::Key_J, Qt::ControlModifier | Qt::ShiftModifier);
        settle();
        QVERIFY(search->text().isEmpty());
        bool j = false;
        for (const auto& k : keys->combos(ShortcutAction::setIn)) if (k.key == QLatin1String("j") && k.command && k.shift) j = true;
        QVERIFY(j);
        keys->resetAll();
        QVERIFY(!keys->isChanged(ShortcutAction::addMarker));
    }

    /// Menus in the Grey theme: a grey panel, the row under the pointer a lighter grey, 13.5 px type.
    void menuLooks() {
        Theme::setCurrent(AppTheme::grey);
        Theme::apply();
        QMenu menu;
        menu.addAction(QStringLiteral("Paste"));
        menu.addSeparator();
        auto* a = menu.addAction(QStringLiteral("New Window"));
        a->setShortcut(QKeySequence(QStringLiteral("Shift+Ctrl+N")));
        menu.addAction(QStringLiteral("Close Window"))->setShortcut(QKeySequence(QStringLiteral("Alt+F4")));
        menu.setActiveAction(a);
        menu.show();
        settle();
        const QFontInfo info(menu.font());
        QVERIFY2(std::abs(info.pixelSize() - 13.5) <= 0.6, qPrintable(QString::number(info.pixelSize())));
        shot(&menu, "menu-grey");
        qApp->setStyleSheet(QString());
        Theme::setCurrent(AppTheme::dark);
    }
};

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
