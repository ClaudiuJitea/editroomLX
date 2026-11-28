// Shortcuts.swift: KeyCombo, ShortcutAction and ShortcutStore, ported literally. Stored in the Mac's JSON
// shape (`{"split":[{"command":true,"control":false,"key":"k","option":false,"shift":false}]}`) under the
// settings key "shortcuts". ⌘ is Ctrl, ⌥ Alt, ⇧ Shift, ⌃ Meta; ⌫ is Backspace and ⌦ Delete.
#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace er {

/// A key and its modifiers, like Ctrl+Shift+S or Space.
struct KeyCombo {
    /// A lowercased character ("s", "=", "\\") or a named key ("space", "delete", "left"…).
    QString key;
    bool command = false; // Ctrl
    bool shift = false;
    bool option = false;  // Alt
    bool control = false; // Meta
    bool operator==(const KeyCombo&) const = default;

    KeyCombo() = default;
    explicit KeyCombo(QString k, bool cmd = false, bool sh = false, bool opt = false, bool ctl = false)
        : key(std::move(k)), command(cmd), shift(sh), option(opt), control(ctl) {}
    static KeyCombo cmd(const QString& k, bool shift = false, bool option = false) { return KeyCombo(k, true, shift, option); }

    /// The combo a key press makes (nil for Escape, which always cancels). `qtKey` is a Qt::Key,
    /// `text` the key's text without modifiers, `modifiers` Qt::KeyboardModifiers.
    static std::optional<KeyCombo> fromKey(int qtKey, const QString& text, int modifiers);
    /// Plain keys and Alt/Shift ones are typing in a text field; Ctrl and Meta ones never are.
    bool typesText() const { return !command && !control; }
    /// Linux style: "Ctrl+Shift+S", "Alt+F", "Backspace", "Space".
    QString display() const;
    /// The Mac's own label ("⌘⇧S"), for comparing with the Mac in a sync.
    QString macDisplay() const;
};

/// Everything that has a keyboard shortcut.
enum class ShortcutAction {
    new_, open, save, saveAs, importMedia, export_,
    newTab, closeTab, previousTab, nextTab,
    undo, redo,
    playPause, stepBack, stepForward, previousEdit, nextEdit, goToStart, goToEnd, loop, fullScreenViewer,
    selectTool, cutTool,
    split, addDissolve, detachAudio, duplicate, freezeFrame, reverse, copyGrade, pasteGrade, delete_, rippleDelete,
    rippleTrimStart, rippleTrimEnd, addText, revealInMedia,
    addMarker, setIn, setOut, clearInOut,
    zoomIn, zoomOut, zoomToFit, snapping, sticky, skimming, audioScrubbing,
};

namespace Shortcut {
const std::vector<ShortcutAction>& allCases();
QString rawValue(ShortcutAction a);
std::optional<ShortcutAction> fromRaw(const QString& s);
QString title(ShortcutAction a);
/// "File", "Edit", "Playback", "Tools", "Clip", "Mark", "View".
QString group(ShortcutAction a);
const std::vector<QString>& groups();
/// Shown in a menu (which shows the first of its shortcuts).
bool inMenu(ShortcutAction a);
std::vector<KeyCombo> defaults(ShortcutAction a);
} // namespace Shortcut

/// The shortcuts in use: the defaults, with the person's changes.
class ShortcutStore {
public:
    static constexpr const char* key = "shortcuts";
    using Load = std::function<QByteArray()>;
    using Save = std::function<void(const std::optional<QByteArray>&)>; // nullopt removes the key
    ShortcutStore(Load load, Save save);

    std::vector<KeyCombo> combos(ShortcutAction a) const;
    bool isChanged(ShortcutAction a) const { return changed_.count(a) > 0; }
    /// For the action's menu item: its first shortcut. Plain Backspace, Space, arrows and the like stay
    /// with the editor window: as a menu shortcut, Backspace in Settings or a dialog would delete clips.
    std::optional<KeyCombo> menu(ShortcutAction a) const;
    /// The action a combo runs, if any.
    std::optional<ShortcutAction> action(const KeyCombo& combo) const;
    /// Menu shortcuts a text field would lose to the menu (plain keys like S): these are typed instead.
    bool isTypedMenuKey(const KeyCombo& combo) const;
    /// Gives an action one shortcut. Another action using it loses it; returns that action.
    std::optional<ShortcutAction> set(ShortcutAction a, const KeyCombo& combo);
    void clear(ShortcutAction a);
    void reset(ShortcutAction a);
    void resetAll();

    static QByteArray encode(const std::map<ShortcutAction, std::vector<KeyCombo>>& changed);
    static std::map<ShortcutAction, std::vector<KeyCombo>> decode(const QByteArray& data);

private:
    void save();
    std::map<ShortcutAction, std::vector<KeyCombo>> changed_;
    Save save_;
};

} // namespace er
