// ContentView.swift + EditroomApp.swift: the editor window. A 38 px title strip (title and dirty dot, the
// export pill, Magic, Export), then Media (252 px, 190–440) | viewer | inspector (320 px) over the timeline
// (360 px, resizable), with the Mac's menus (File, Edit, Clip, Mark, View, Help) and shortcuts.
#pragma once
#include "Controller.h"
#include "model/Shortcuts.h"
#include <QMainWindow>

class QLabel;
class QSplitter;

namespace er {

class LibraryPanel;
class PreviewPanel;
class InspectorPanel;
class TimelinePanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Controller& c, QWidget* parent = nullptr);
    ShortcutStore& keys() { return keys_; }
    void rebuildMenus();
    void applyTheme();

signals:
    /// The Magic button (the Magic panel lives with the MCP server and agents).
    void magicRequested();

protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    bool runShortcut(QKeyEvent* e, bool inTextField);
    void perform(ShortcutAction a);
    void refreshTitle();
    /// Viewer only: everything but the viewer is put away and the window covers the screen; leaving puts the
    /// window back as it was.
    void setViewerOnly(bool on);
    bool wasFullScreen_ = false;
    QWidget* strip_ = nullptr;
    QWidget* magicButton_ = nullptr;
    void showExport();
    void showSettings();
    void showAbout();
    Controller& c_;
    ShortcutStore keys_;
    QLabel* title_ = nullptr;
    QLabel* exportPill_ = nullptr;
    LibraryPanel* library_ = nullptr;
    PreviewPanel* preview_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    TimelinePanel* timeline_ = nullptr;
    QSplitter* top_ = nullptr;
    QSplitter* vertical_ = nullptr;
};

} // namespace er
