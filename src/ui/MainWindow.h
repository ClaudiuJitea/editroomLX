// ContentView.swift + EditroomApp.swift: the editor window. A 38 px title strip (the project tabs, the
// export pill, Magic, Export), then Media (252 px, 190–440) | viewer | inspector (320 px) over the timeline
// (360 px, resizable), with the Mac's menus (File, Edit, Clip, Mark, View, Help) and shortcuts.
#pragma once
#include "Controller.h"
#include "Workspace.h"
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
    explicit MainWindow(Workspace& w, QWidget* parent = nullptr);
    ShortcutStore& keys() { return keys_; }
    Workspace& workspace() { return w_; }
    /// The tab in front's editor.
    Controller& controller() { return *c_; }
    TimelinePanel* timeline() const { return timeline_; }
    void rebuildMenus();
    void applyTheme();

signals:
    /// The Magic button (the Magic panel lives with the MCP server and agents).
    void magicRequested();
    /// Another tab came to the front: its editor is the one the menus, Magic and the AI apps act on.
    void controllerChanged(Controller& c);

protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    /// The tab in front's panels, made again when another tab comes to the front (the title strip stays).
    void buildEditor();
    void tearDownEditor();
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
    Workspace& w_;
    Controller* c_ = nullptr;
    ShortcutStore keys_;
    QWidget* editor_ = nullptr;
    QWidget* tabs_ = nullptr;
    QLabel* exportPill_ = nullptr;
    LibraryPanel* library_ = nullptr;
    PreviewPanel* preview_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    TimelinePanel* timeline_ = nullptr;
    QSplitter* top_ = nullptr;
    QSplitter* vertical_ = nullptr;
};

} // namespace er
