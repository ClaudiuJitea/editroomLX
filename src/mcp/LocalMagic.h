// Local Magic (MagicPopover.swift's LocalMagic and MagicShow.start): a cut made on this computer, with no network
// and no key. With a request, LocalBrain reads it into a recipe first. With nothing on the timeline, AutoPlanner
// makes a new cut from the library; with an edit there, LocalImprover improves it. The edit is applied at once,
// as one undo step (the Mac's cursor show is docs/PARITY.md's pending row).
#pragma once
#include "MagicPlan.h"
#include "analysis/LocalBrain.h"
#include <QElapsedTimer>
#include <QObject>
#include <QThreadPool>

namespace er {

class Controller;
struct LocalOutcome;

class LocalMagic : public QObject {
    Q_OBJECT
public:
    /// The one for this window's controller.
    static LocalMagic& of(Controller& c);
    explicit LocalMagic(Controller& c);
    ~LocalMagic() override;

    bool busy() const { return busy_; }
    QString status() const { return status_; }
    /// The Local tab's Make: the request read, then the edit made (or a note on why not).
    void run(const QString& request);
    /// MagicShow.start: looks at the media, plans a cut and makes it. `done` gets what was done and whether
    /// anything changed.
    void start(const LocalRecipe& recipe, std::function<void(QString, bool)> done = {});
    /// Stop: while it's still working the edit out, nothing has changed.
    void stop();

    /// MagicShow.madeNote.
    static QString madeNote(const EditDecision& d, double length, const LocalRecipe& recipe);

signals:
    void statusChanged();
    void finished(const QString& said, bool changed);

private:
    void hear(const std::vector<Uuid>& assets, std::function<void()> then);
    /// The thinking scene, begun if it isn't on.
    void scene();
    /// A plan played: sketched by the thinking scene first when it's on, then the cursors.
    void play(const MagicPlan& plan, std::function<void(bool)> done);
    QElapsedTimer sceneClock_;
    void perform(const LocalOutcome& outcome);
    void captionCut(const QString& said);
    void emitNote(const QString& said, bool changed);
    bool captionsAfter_ = false;
    void setStatus(const QString& s);
    void finish(const QString& said, bool changed);
    Controller& c_;
    bool busy_ = false;
    QString status_;
    int run_ = 0;
    QThreadPool pool_;
};

} // namespace er
