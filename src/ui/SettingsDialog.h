// SettingsView.swift: Settings (Ctrl+,). Its own tab row (icon over name): General (the themes, each a small
// picture of the editor in its colours; Media as Thumbnails or List; Show time as; Auto-save), Shortcuts
// (search; every action by group with its keys; click to record, right-click for Change, Remove, Restore
// Default; where Space plays from while skimming; Restore Defaults), Magic (Enable Magic, Show Magic at work, the
// AI apps to connect, letting AI apps edit here and who's working now) and Updates. Kept under the Mac's keys.
#pragma once
#include "Controller.h"
#include "model/Shortcuts.h"
#include <QDateTime>
#include <QDialog>

class QStackedWidget;
class QLabel;
class QVBoxLayout;

namespace er {

/// An app Editroom can connect to (Settings › Magic › AI apps), given by the app's wiring.
struct AgentApp {
    QString name;
    std::function<bool()> installed, connected;
    std::function<bool(QString*)> connect, disconnect;
    /// What the row says, by state.
    QString readyNote, connectedNote;
};

/// The MCP server, as Settings sees it (it lives in another library; the app wires these).
struct AgentServerHooks {
    std::function<bool()> enabled;
    std::function<void(bool)> setEnabled;
    std::function<QString()> problem;
    /// Agents that did something in the last minute: their names, newest first.
    std::function<QStringList()> active;
    /// Where agents reach the server (Streamable HTTP), for Copy Prompt.
    std::function<QString()> endpoint;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    static std::vector<AgentApp>& agentApps();
    static AgentServerHooks& server();
    SettingsDialog(Controller& c, ShortcutStore& keys, QWidget* parent = nullptr);
signals:
    void themeChanged();
    void shortcutsChanged();
    void magicEnabledChanged(bool on);
protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;
private:
    enum class Tab { general, shortcuts, magic, updates };
    void showTab(Tab t);
    QWidget* general();
    QWidget* shortcuts();
    QWidget* magic();
    QWidget* updates();
    void fillShortcuts();
    void setNote(const QString& note);
    Controller& c_;
    ShortcutStore& keys_;
    QWidget* tabRow_;
    QLabel* title_;
    QStackedWidget* pages_;
    Tab tab_ = Tab::shortcuts;
    // Shortcuts
    QVBoxLayout* shortcutList_ = nullptr;
    QString shortcutSearch_;
    std::optional<ShortcutAction> recording_;
    QLabel* shortcutNote_ = nullptr;
    QString note_;
    QWidget* restoreDefaults_ = nullptr;
};

} // namespace er
