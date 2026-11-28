// Workspace.swift: the projects open in the window, one to a tab. Only the tab in front plays, rebuilds and talks to
// the AI apps; the others are paused with their player let go, so a tab behind costs next to nothing. Tabs reopen at
// launch, each with its own recovery copy; ones behind load when first shown (LX doesn't even make their editor until
// then). Kept under the Mac's keys: "tabSlots" and "activeTab".
#pragma once
#include "Controller.h"
#include <QObject>
#include <functional>
#include <memory>

namespace er {

class Workspace : public QObject {
    Q_OBJECT
public:
    static constexpr const char* tabsKey = "tabSlots";
    static constexpr const char* activeKey = "activeTab";

    /// `persists`: false for test launches (`--open`, `--selftest`), which open one tab and leave the saved tabs alone.
    explicit Workspace(bool persists, QObject* parent = nullptr);
    ~Workspace() override;

    int count() const { return int(tabs_.size()); }
    int activeIndex() const { return active_; }
    /// The tab in front's editor (made if it hasn't been yet).
    Controller& active();
    /// A tab's editor, or none while it hasn't been shown since launch.
    Controller* loaded(int i) const { return i >= 0 && i < count() ? tabs_[size_t(i)].c.get() : nullptr; }
    const QString& slot(int i) const { return tabs_[size_t(i)].slot; }

    /// For its tab: the project's name, whether it has unsaved changes (including ones waiting in a tab not reopened
    /// yet), and the file open in it (or to be, once it's reopened).
    QString title(int i) const;
    bool showsUnsaved(int i) const;
    QString openPath(int i) const;
    bool isBlank(int i) const;

    void select(int i);
    void selectNext() { step(1); }
    void selectPrevious() { step(-1); }
    /// A new, empty tab, just after the one in front.
    void newTab();
    /// Opens a project: in front if it's already open, else in the tab in front when that's empty, else in a new tab.
    void open(const QString& path);
    /// Files opened from the file manager or handed to a second launch. Projects open in tabs; media goes into the tab
    /// in front, or, for files that opened Editroom, into a new tab unless the one in front is empty.
    void openFromOutside(const QStringList& files, bool atLaunch);
    /// Closes a tab, asking first about unsaved work as closing the project would. The last tab stays, with its project
    /// closed and an empty Untitled one in its place.
    void close(int i);
    void closeActive() { close(active_); }
    void closeOthers(int keep);
    /// Close Tab does something: there's another tab, or a project in this one.
    bool canClose();
    /// Each tab with unsaved changes that need saving asks, in front, before Editroom quits.
    bool confirmQuit();
    void autosaveAll();

    /// While Magic works on the tab in front, it stays in front (the app's wiring says when).
    static std::function<bool()>& lockedCheck();
    /// What a new tab's editor is set up with (the app's wiring: the window, autosave).
    std::function<void(Controller&)> setUp;

signals:
    /// The tab in front is about to change (its editor's views should go).
    void aboutToSwitch();
    /// The tab in front changed (or its editor was made): the window builds its views for it.
    void activeChanged(Controller* now);
    /// Tabs added, removed, renamed or marked.
    void tabsChanged();

private:
    struct Tab {
        QString slot;
        std::unique_ptr<Controller> c;
    };
    Controller& ensure(int i, bool restore);
    void step(int by);
    void remember() const;
    bool locked() const { return lockedCheck() && lockedCheck()(); }
    void watch(Controller& c);
    std::vector<Tab> tabs_;
    int active_ = 0;
    bool persists_;
};

} // namespace er
