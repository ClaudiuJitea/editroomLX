#include "Workspace.h"
#include <QFileInfo>
#include <QSettings>
#include <QUuid>

namespace er {

std::function<bool()>& Workspace::lockedCheck() {
    static std::function<bool()> f;
    return f;
}

Workspace::Workspace(bool persists, QObject* parent) : QObject(parent), persists_(persists) {
    QStringList saved;
    if (persists_) saved = QSettings().value(QLatin1String(tabsKey)).toStringList();
    if (saved.isEmpty()) saved = {QString()};
    for (const QString& s : saved) tabs_.push_back({s, nullptr});
    const QString front = persists_ ? QSettings().value(QLatin1String(activeKey)).toString() : QString();
    active_ = std::max(0, int(saved.indexOf(front)));
}

Workspace::~Workspace() {
    // Editors go in reverse, the one in front last (its views went with the window).
    while (!tabs_.empty()) tabs_.pop_back();
}

Controller& Workspace::ensure(int i, bool restore) {
    Tab& t = tabs_[size_t(i)];
    if (t.c) return *t.c;
    t.c = std::make_unique<Controller>();
    Controller& c = *t.c;
    c.model().setSlot(t.slot);
    c.model().testLaunch = !persists_;
    if (setUp) setUp(c);
    watch(c);
    // What it had open last time is reopened when it's first shown.
    if (restore && persists_) c.model().restoreLastSession();
    if (persists_) c.model().startAutosaveTimer();
    if (i != active_) c.suspend();
    return c;
}

void Workspace::watch(Controller& c) {
    // Its name and dot are on its tab.
    connect(&c.model(), &EditorModel::changed, this, &Workspace::tabsChanged);
}

Controller& Workspace::active() { return ensure(active_, true); }

QString Workspace::title(int i) const {
    if (Controller* c = loaded(i)) return c->model().title();
    const auto last = EditorModel::savedSession(slot(i));
    if (last && *last != QLatin1String("untitled")) return QFileInfo(*last).completeBaseName();
    return QStringLiteral("Untitled");
}

bool Workspace::showsUnsaved(int i) const {
    if (Controller* c = loaded(i)) return c->model().isDirty();
    const auto last = EditorModel::savedSession(slot(i));
    if (!last) return false;
    return *last == QLatin1String("untitled") || EditorModel::savedRecoveryFor(slot(i)) == *last;
}

QString Workspace::openPath(int i) const {
    if (Controller* c = loaded(i)) return c->model().openPath();
    const auto last = EditorModel::savedSession(slot(i));
    if (last && *last != QLatin1String("untitled")) return QFileInfo(*last).absoluteFilePath();
    return {};
}

bool Workspace::isBlank(int i) const {
    if (Controller* c = loaded(i)) return c->model().isBlank();
    return !EditorModel::savedSession(slot(i));
}

void Workspace::remember() const {
    if (!persists_) return;
    QStringList saved;
    for (const auto& t : tabs_) saved << t.slot;
    QSettings s;
    s.setValue(QLatin1String(tabsKey), saved);
    s.setValue(QLatin1String(activeKey), tabs_[size_t(active_)].slot);
}

void Workspace::select(int i) {
    if (i < 0 || i >= count() || i == active_ || locked()) return;
    emit aboutToSwitch();
    if (Controller* now = loaded(active_)) now->suspend();
    active_ = i;
    Controller& c = ensure(i, true);
    c.resume();
    remember();
    emit activeChanged(&c);
    emit tabsChanged();
}

void Workspace::step(int by) {
    if (count() < 2) return;
    select((active_ + by + count()) % count());
}

void Workspace::newTab() {
    if (locked()) return;
    const QString s = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toUpper();
    const int at = active_ + 1;
    tabs_.insert(tabs_.begin() + at, Tab{s, nullptr});
    // The tab in front moved along if the new one went before it (it never does: it goes just after it).
    select(at);
}

void Workspace::open(const QString& path) {
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < count(); ++i) if (openPath(i) == abs) { select(i); return; }
    if (!QFileInfo::exists(abs)) {
        active().showError(QStringLiteral("“%1” can't be found. It may have been moved or deleted.").arg(QFileInfo(path).fileName()));
        return;
    }
    if (locked()) return;
    if (!isBlank(active_)) newTab();
    active().model().open(abs);
}

void Workspace::openFromOutside(const QStringList& files, bool atLaunch) {
    QStringList media;
    for (const QString& f : files) {
        if (EditorModel::isProjectFile(f)) open(f);
        else media << f;
    }
    if (media.isEmpty()) return;
    if (atLaunch && !isBlank(active_)) newTab();
    for (const QString& f : media) active().importFiles({f});
}

void Workspace::close(int i) {
    if (i < 0 || i >= count()) return;
    if (count() == 1) {
        if (locked()) return;
        active().model().closeProject();
        emit tabsChanged();
        return;
    }
    if (locked() && i == active_) return;
    // Unsaved work in it is put in front before asking about it.
    Controller& c = ensure(i, true);
    if (c.model().isDirty()) select(i);
    if (!c.model().confirmDiscard()) return;
    const bool wasActive = i == active_;
    if (wasActive) emit aboutToSwitch();
    c.suspend();
    c.model().forgetSlot();
    tabs_.erase(tabs_.begin() + i);
    if (wasActive) {
        active_ = std::min(i, count() - 1);
        Controller& next = ensure(active_, true);
        next.resume();
        remember();
        emit activeChanged(&next);
    } else {
        if (i < active_) --active_;
        remember();
    }
    emit tabsChanged();
}

void Workspace::closeOthers(int keep) {
    if (keep < 0 || keep >= count()) return;
    const QString kept = slot(keep);
    for (int i = count() - 1; i >= 0; --i) if (slot(i) != kept) close(i);
}

bool Workspace::canClose() { return count() > 1 || !isBlank(active_); }

bool Workspace::confirmQuit() {
    std::vector<int> order{active_};
    for (int i = 0; i < count(); ++i) if (i != active_) order.push_back(i);
    for (int i : order) {
        Controller* c = loaded(i);
        if (!c) continue;
        if (!EditorModel::autosaves() && c->model().isDirty()) select(i);
        if (!c->model().confirmDiscard(true)) return false;
    }
    return true;
}

void Workspace::autosaveAll() {
    for (auto& t : tabs_) if (t.c) t.c->model().autosaveNow();
}

} // namespace er
