#include "Shortcuts.h"
#include "Json.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <Qt>
#include <algorithm>

namespace er {

using A = ShortcutAction;

std::optional<KeyCombo> KeyCombo::fromKey(int qtKey, const QString& text, int modifiers) {
    if (qtKey == Qt::Key_Escape) return std::nullopt;
    static const std::map<int, QString> named = {
        {Qt::Key_Space, "space"}, {Qt::Key_Backspace, "delete"}, {Qt::Key_Delete, "forwardDelete"},
        {Qt::Key_Left, "left"}, {Qt::Key_Right, "right"}, {Qt::Key_Down, "down"}, {Qt::Key_Up, "up"},
        {Qt::Key_Home, "home"}, {Qt::Key_End, "end"}, {Qt::Key_Return, "return"}, {Qt::Key_Enter, "return"},
        {Qt::Key_Tab, "tab"}, {Qt::Key_Backtab, "tab"}, {Qt::Key_PageUp, "pageUp"}, {Qt::Key_PageDown, "pageDown"},
    };
    KeyCombo c;
    if (auto it = named.find(qtKey); it != named.end()) {
        c.key = it->second;
    } else if (!text.isEmpty() && text[0].isPrint()) {
        c.key = text.toLower();
    } else if (qtKey >= 0x20 && qtKey < 0x7f) {
        // With Ctrl held, Qt's text is a control character: use the key itself, as the Mac's
        // charactersIgnoringModifiers does.
        c.key = QString(QChar(qtKey)).toLower();
    } else {
        return std::nullopt;
    }
    c.command = modifiers & Qt::ControlModifier;
    c.shift = modifiers & Qt::ShiftModifier;
    c.option = modifiers & Qt::AltModifier;
    c.control = modifiers & Qt::MetaModifier;
    return c;
}

static QString keyLabel(const QString& key, bool mac) {
    static const std::map<QString, std::pair<QString, QString>> names = {
        {"space", {"Space", "Space"}}, {"delete", {"⌫", "Backspace"}}, {"forwardDelete", {"⌦", "Delete"}},
        {"left", {"←", "←"}}, {"right", {"→", "→"}}, {"up", {"↑", "↑"}}, {"down", {"↓", "↓"}},
        {"home", {"Home", "Home"}}, {"end", {"End", "End"}}, {"return", {"↩", "Enter"}}, {"tab", {"⇥", "Tab"}},
        {"pageUp", {"Page Up", "Page Up"}}, {"pageDown", {"Page Down", "Page Down"}},
    };
    if (auto it = names.find(key); it != names.end()) return mac ? it->second.first : it->second.second;
    return key.toUpper();
}

QString KeyCombo::display() const {
    // Qt's order on Linux: Meta, Ctrl, Alt, Shift.
    QString s;
    if (control) s += QStringLiteral("Meta+");
    if (command) s += QStringLiteral("Ctrl+");
    if (option) s += QStringLiteral("Alt+");
    if (shift) s += QStringLiteral("Shift+");
    return s + keyLabel(key, false);
}

QString KeyCombo::macDisplay() const {
    QString s;
    if (control) s += QStringLiteral("⌃");
    if (option) s += QStringLiteral("⌥");
    if (shift) s += QStringLiteral("⇧");
    if (command) s += QStringLiteral("⌘");
    return s + keyLabel(key, true);
}

namespace {
struct Row { A a; const char* raw; const char* title; const char* group; };
const Row rows[] = {
    {A::new_, "new", "New", "File"}, {A::open, "open", "Open…", "File"}, {A::save, "save", "Save", "File"},
    {A::saveAs, "saveAs", "Save As…", "File"}, {A::importMedia, "importMedia", "Import Media…", "File"},
    {A::export_, "export", "Export…", "File"},
    {A::undo, "undo", "Undo", "Edit"}, {A::redo, "redo", "Redo", "Edit"},
    {A::playPause, "playPause", "Play / Pause", "Playback"}, {A::stepBack, "stepBack", "Back One Frame", "Playback"},
    {A::stepForward, "stepForward", "Forward One Frame", "Playback"}, {A::previousEdit, "previousEdit", "Previous Edit", "Playback"},
    {A::nextEdit, "nextEdit", "Next Edit", "Playback"}, {A::goToStart, "goToStart", "Go to Start", "Playback"},
    {A::goToEnd, "goToEnd", "Go to End", "Playback"}, {A::loop, "loop", "Loop Playback", "Playback"},
    {A::fullScreenViewer, "fullScreenViewer", "Full Screen Viewer", "Playback"},
    {A::selectTool, "selectTool", "Select Tool", "Tools"}, {A::cutTool, "cutTool", "Cut Tool", "Tools"},
    {A::split, "split", "Split at Playhead", "Clip"}, {A::addDissolve, "addDissolve", "Add Dissolve", "Clip"},
    {A::detachAudio, "detachAudio", "Detach Audio", "Clip"}, {A::duplicate, "duplicate", "Duplicate", "Clip"},
    {A::freezeFrame, "freezeFrame", "Freeze Frame", "Clip"}, {A::reverse, "reverse", "Reverse", "Clip"},
    {A::copyGrade, "copyGrade", "Copy Grade", "Clip"}, {A::pasteGrade, "pasteGrade", "Paste Grade", "Clip"},
    {A::delete_, "delete", "Delete", "Clip"}, {A::rippleDelete, "rippleDelete", "Ripple Delete", "Clip"},
    {A::rippleTrimStart, "rippleTrimStart", "Ripple Trim Start to Playhead", "Clip"},
    {A::rippleTrimEnd, "rippleTrimEnd", "Ripple Trim End to Playhead", "Clip"}, {A::addText, "addText", "Add Text", "Clip"},
    {A::revealInMedia, "revealInMedia", "Reveal in Media", "Clip"},
    {A::addMarker, "addMarker", "Add Marker", "Mark"}, {A::setIn, "setIn", "Set In", "Mark"},
    {A::setOut, "setOut", "Set Out", "Mark"}, {A::clearInOut, "clearInOut", "Clear In and Out", "Mark"},
    {A::zoomIn, "zoomIn", "Zoom In", "View"}, {A::zoomOut, "zoomOut", "Zoom Out", "View"},
    {A::zoomToFit, "zoomToFit", "Zoom to Fit", "View"}, {A::snapping, "snapping", "Snapping", "View"},
    {A::sticky, "sticky", "Sticky Edges", "View"}, {A::skimming, "skimming", "Skimming", "View"},
    {A::audioScrubbing, "audioScrubbing", "Audio Scrubbing", "View"},
};
const Row& row(A a) { for (const auto& r : rows) if (r.a == a) return r; return rows[0]; }
} // namespace

const std::vector<A>& Shortcut::allCases() {
    static const std::vector<A> all = [] { std::vector<A> v; for (const auto& r : rows) v.push_back(r.a); return v; }();
    return all;
}
QString Shortcut::rawValue(A a) { return QString::fromLatin1(row(a).raw); }
std::optional<A> Shortcut::fromRaw(const QString& s) {
    for (const auto& r : rows) if (s == QLatin1String(r.raw)) return r.a;
    return std::nullopt;
}
QString Shortcut::title(A a) { return QString::fromUtf8(row(a).title); }
QString Shortcut::group(A a) { return QString::fromLatin1(row(a).group); }
const std::vector<QString>& Shortcut::groups() {
    static const std::vector<QString> g{"File", "Edit", "Playback", "Tools", "Clip", "Mark", "View"};
    return g;
}

bool Shortcut::inMenu(A a) {
    switch (a) {
    case A::playPause: case A::stepBack: case A::stepForward: case A::previousEdit: case A::nextEdit:
    case A::goToStart: case A::goToEnd: case A::loop: case A::fullScreenViewer: case A::selectTool: case A::cutTool:
        return false;
    default: return true;
    }
}

std::vector<KeyCombo> Shortcut::defaults(A a) {
    using K = KeyCombo;
    auto cmd = [](const char* k, bool shift = false, bool option = false) { return K::cmd(QString::fromLatin1(k), shift, option); };
    auto key = [](const char* k, bool shift = false, bool option = false) { return K(QString::fromLatin1(k), false, shift, option); };
    switch (a) {
    case A::new_: return {cmd("n")};
    case A::open: return {cmd("o")};
    case A::save: return {cmd("s")};
    case A::saveAs: return {cmd("s", true)};
    case A::importMedia: return {cmd("i")};
    case A::export_: return {cmd("e")};
    case A::undo: return {cmd("z")};
    case A::redo: return {cmd("z", true)};
    case A::playPause: return {key("space")};
    case A::stepBack: return {key("left")};
    case A::stepForward: return {key("right")};
    case A::previousEdit: return {key("up"), key("left", true)};
    case A::nextEdit: return {key("down"), key("right", true)};
    case A::goToStart: return {key("home")};
    case A::goToEnd: return {key("end")};
    case A::loop: return {key("l")};
    case A::fullScreenViewer: return {key("f")};
    case A::selectTool: return {key("v"), key("a")};
    case A::cutTool: return {key("c")};
    case A::split: return {cmd("k")};
    case A::addDissolve: return {cmd("d")};
    case A::detachAudio: return {cmd("l", true)};
    case A::duplicate: return {cmd("d", true)};
    case A::freezeFrame: return {key("f", false, true)};
    case A::reverse: return {key("r", false, true)};
    case A::copyGrade: return {cmd("c", false, true)};
    case A::pasteGrade: return {cmd("v", false, true)};
    case A::delete_: return {key("delete"), key("forwardDelete")};
    case A::rippleDelete: return {key("delete", true), key("forwardDelete", true)};
    case A::rippleTrimStart: return {key("q")};
    case A::rippleTrimEnd: return {key("w")};
    case A::addText: return {cmd("t")};
    // As in Final Cut Pro (Reveal in Browser).
    case A::revealInMedia: return {KeyCombo(QStringLiteral("f"), false, true)};
    case A::addMarker: return {key("m")};
    case A::setIn: return {key("i")};
    case A::setOut: return {key("o")};
    case A::clearInOut: return {key("x", true)};
    case A::zoomIn: return {cmd("="), key("="), key("+", true)};
    case A::zoomOut: return {cmd("-"), key("-")};
    case A::zoomToFit: return {cmd("0"), key("z", true), key("\\")};
    case A::snapping: return {key("s")};
    case A::sticky: return {key("r")};
    case A::skimming: return {key("k")};
    case A::audioScrubbing: return {key("s", true)};
    }
    return {};
}

// MARK: Store

ShortcutStore::ShortcutStore(Load load, Save save) : save_(std::move(save)) {
    if (load) changed_ = decode(load());
}

std::vector<KeyCombo> ShortcutStore::combos(A a) const {
    if (auto it = changed_.find(a); it != changed_.end()) return it->second;
    return Shortcut::defaults(a);
}

std::optional<KeyCombo> ShortcutStore::menu(A a) const {
    const auto cs = combos(a);
    if (cs.empty()) return std::nullopt;
    const KeyCombo& c = cs.front();
    if (!c.command && !c.control && !c.option && c.key.size() > 1) return std::nullopt;
    return c;
}

std::optional<A> ShortcutStore::action(const KeyCombo& combo) const {
    for (A a : Shortcut::allCases()) {
        const auto cs = combos(a);
        if (std::find(cs.begin(), cs.end(), combo) != cs.end()) return a;
    }
    return std::nullopt;
}

bool ShortcutStore::isTypedMenuKey(const KeyCombo& combo) const {
    if (!combo.typesText()) return false;
    for (A a : Shortcut::allCases()) {
        if (!Shortcut::inMenu(a)) continue;
        const auto cs = combos(a);
        if (!cs.empty() && cs.front() == combo) return true;
    }
    return false;
}

std::optional<A> ShortcutStore::set(A a, const KeyCombo& combo) {
    std::optional<A> taken;
    for (A other : Shortcut::allCases()) {
        if (other == a) continue;
        auto cs = combos(other);
        if (std::find(cs.begin(), cs.end(), combo) == cs.end()) continue;
        std::erase(cs, combo);
        changed_[other] = cs;
        taken = other;
    }
    changed_[a] = {combo};
    save();
    return taken;
}

void ShortcutStore::clear(A a) { changed_[a] = {}; save(); }
void ShortcutStore::reset(A a) { changed_.erase(a); save(); }
void ShortcutStore::resetAll() { changed_.clear(); save(); }

void ShortcutStore::save() {
    if (!save_) return;
    if (changed_.empty()) save_(std::nullopt);
    else save_(encode(changed_));
}

QByteArray ShortcutStore::encode(const std::map<A, std::vector<KeyCombo>>& changed) {
    QJsonObject o;
    for (const auto& [a, list] : changed) {
        QJsonArray arr;
        for (const auto& c : list)
            arr.append(QJsonObject{{"key", c.key}, {"command", c.command}, {"shift", c.shift}, {"option", c.option}, {"control", c.control}});
        o.insert(Shortcut::rawValue(a), arr);
    }
    return json::write(o, false);
}

std::map<A, std::vector<KeyCombo>> ShortcutStore::decode(const QByteArray& data) {
    std::map<A, std::vector<KeyCombo>> out;
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const auto a = Shortcut::fromRaw(it.key());
        if (!a || !it.value().isArray()) continue;
        std::vector<KeyCombo> list;
        bool ok = true;
        for (const auto& v : it.value().toArray()) {
            const QJsonObject c = v.toObject();
            // Swift's synthesised decoder needs every field; a combo missing one makes the whole value unreadable.
            for (const char* k : {"key", "command", "shift", "option", "control"}) if (!c.contains(QLatin1String(k))) ok = false;
            list.push_back(KeyCombo(c.value("key").toString(), c.value("command").toBool(), c.value("shift").toBool(),
                                    c.value("option").toBool(), c.value("control").toBool()));
        }
        if (!ok) return {};
        out[*a] = list;
    }
    return out;
}

} // namespace er
