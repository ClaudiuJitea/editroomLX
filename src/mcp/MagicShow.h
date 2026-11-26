// MagicShow.swift, its core: Magic's edits played on the timeline by cursors instead of all at once. Each task
// waits for the ones it comes after (and its small stagger); a free cursor flies to where the edit happens and
// makes it. Local Magic is a little swarm; an AI agent connected over MCP is one named cursor in its own colour.
// The project changes live as they work and the whole show is one undo step; Esc puts everything back. Off in
// Settings › Magic › Show Magic at work, or with no timeline on screen, the edit is made at once.
// (The Mac's thinking scene, scans and sketch before the swarm are not drawn here; see docs/PARITY.md.)
#pragma once
#include "MagicPlan.h"
#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace er {

class Controller;
class TimelineCanvas;

class MagicShow : public QWidget {
    Q_OBJECT
public:
    struct Performer { QString name; QColor color; };
    /// Settings › Magic › Show Magic at work (on unless turned off).
    static bool animated();
    static bool running();
    /// Plays a plan (or makes it at once when it can't be shown). `done` runs when it's over, with whether it was
    /// finished (false: stopped, everything put back).
    static void play(Controller& c, const MagicPlan& plan, std::function<void(bool)> done, std::optional<Performer> performer = std::nullopt);
    /// Stop (Esc or the Magic button): mid-edit, everything the cursors did is put back.
    static void stopAll();
    /// A colour for an agent, by name (Claude's orange, ChatGPT's green, else one of a few).
    static QColor color(const QString& who);

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    MagicShow(Controller& c, TimelineCanvas* canvas, const MagicPlan& plan, std::function<void(bool)> done, std::optional<Performer> performer);
    ~MagicShow() override;
    struct Cursor {
        QPointF at, from, to;
        int task = -1;
        double t0 = 0, travel = 0;
        bool clicking = false;
        QColor color;
        QString name;
    };
    void step();
    QPointF target(const MagicTask& task) const;
    void finish(bool finished);
    double now() const { return clock_.elapsed() / 1000.0; }
    Controller& c_;
    QPointer<TimelineCanvas> canvas_;
    MagicPlan plan_;
    std::function<void(bool)> done_;
    std::vector<Cursor> cursors_;
    std::vector<double> doneAt_;     // when each task was made (negative: not yet)
    std::vector<bool> taken_;
    QTimer timer_;
    QElapsedTimer clock_;
    double endedAt_ = -1;
};

} // namespace er
