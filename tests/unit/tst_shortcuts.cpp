// The SHORTCUTS check from SelfTest.swift, with Qt key events in place of NSEvents.
#include "model/Shortcuts.h"
#include <QTest>

using namespace er;
using A = ShortcutAction;

class TestShortcuts : public QObject {
    Q_OBJECT
    std::optional<QByteArray> stored;
    ShortcutStore store() {
        return ShortcutStore([this] { return stored.value_or(QByteArray()); }, [this](const std::optional<QByteArray>& d) { stored = d; });
    }
private slots:
    void shortcuts() {
        stored.reset();
        auto keys = store();
        auto press = [&](int key, const QString& text, int mods = 0) { auto c = KeyCombo::fromKey(key, text, mods); return c ? keys.action(*c) : std::optional<A>(); };
        QCOMPARE(press(Qt::Key_Space, " "), std::optional(A::playPause));
        QCOMPARE(press(Qt::Key_S, "S", Qt::ShiftModifier), std::optional(A::audioScrubbing));
        QCOMPARE(press(Qt::Key_S, QString(QChar(0x13)), Qt::ControlModifier), std::optional(A::save));
        QCOMPARE(press(Qt::Key_Backspace, "\b", Qt::ShiftModifier), std::optional(A::rippleDelete));
        QVERIFY(!KeyCombo::fromKey(Qt::Key_Escape, "\x1b", 0));
        // P for Snapping; then S (Snapping's old key) for Loop, which has to take it from nobody.
        keys.set(A::snapping, KeyCombo("p"));
        const auto taken = keys.set(A::loop, KeyCombo("s"));
        QVERIFY(keys.action(KeyCombo("p")) == A::snapping && keys.action(KeyCombo("s")) == A::loop && !taken);
        // Shift+S for Loop takes it from Audio Scrubbing.
        const auto stolen = keys.set(A::loop, KeyCombo("s", false, true));
        QVERIFY(stolen == A::audioScrubbing && keys.combos(A::audioScrubbing).empty());
        auto reloaded = store();
        QCOMPARE(reloaded.combos(A::snapping), std::vector<KeyCombo>{KeyCombo("p")});
        reloaded.resetAll();
        QVERIFY(!stored);
        QCOMPARE(store().combos(A::snapping), Shortcut::defaults(A::snapping));
        // Menus never get plain Delete (it would delete clips from Settings or a dialog).
        QVERIFY(!reloaded.menu(A::delete_) && reloaded.menu(A::save));
    }

    void storedShape() {
        // The Mac's JSON shape, so the two stay comparable.
        const QByteArray mac = "{\"split\":[{\"command\":true,\"control\":false,\"key\":\"j\",\"option\":false,\"shift\":false}]}";
        stored = mac;
        auto keys = store();
        QCOMPARE(keys.combos(A::split), std::vector<KeyCombo>{KeyCombo::cmd("j")});
        keys.set(A::split, KeyCombo::cmd("j"));
        QCOMPARE(*stored, QByteArray("{\"split\":[{\"command\":true,\"control\":false,\"key\":\"j\",\"option\":false,\"shift\":false}]}"));
    }

    void display() {
        QCOMPARE(KeyCombo::cmd("s", true).display(), QStringLiteral("Ctrl+Shift+S"));
        QCOMPARE(KeyCombo("f", false, false, true).display(), QStringLiteral("Alt+F"));
        QCOMPARE(KeyCombo("delete").display(), QStringLiteral("Backspace"));
        QCOMPARE(KeyCombo("forwardDelete", false, true).display(), QStringLiteral("Shift+Delete"));
        QCOMPARE(KeyCombo::cmd("s", true).macDisplay(), QStringLiteral("⇧⌘S"));
        QCOMPARE(Shortcut::title(A::open), QStringLiteral("Open…"));
        QCOMPARE(Shortcut::allCases().size(), size_t(44)); // with Reveal in Media (1.0.10)
    }

    void typedMenuKeys() {
        stored.reset();
        auto keys = store();
        QVERIFY(keys.isTypedMenuKey(KeyCombo("s")));      // Snapping is in the View menu
        QVERIFY(!keys.isTypedMenuKey(KeyCombo("space"))); // Play / Pause isn't in a menu
        QVERIFY(!keys.isTypedMenuKey(KeyCombo::cmd("s")));
    }
};

QTEST_GUILESS_MAIN(TestShortcuts)
#include "tst_shortcuts.moc"
