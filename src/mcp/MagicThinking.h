// MagicThinking.swift: the show while Magic works out an edit, before the cursors make it. The window dims, all but
// the Magic button (Stop); Esc cancels. Three stages, only ever forwards:
//  - Looking: scans loop over the items in the Media panel;
//  - Analyzing: scans loop over the clips, the Media items and the viewer, a few at a time, each with small corner
//    brackets that fade once its scan is done;
//  - Thinking: a field of drifting dots shimmers over the viewer while the coming edit appears as dashed outlines
//    on the timeline.
// Then it fades out where it is and the cursors take over; each outline stays until its clip lands. Meanwhile the
// editor is locked (MagicLock): clicks, scrolling and keys do nothing, bar Esc, the Magic button, Space (play) and
// quitting.
#pragma once
#include "model/Project.h"
#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

namespace er {

class Controller;

class MagicThinking : public QWidget {
    Q_OBJECT
public:
    enum class Stage { looking, analyzing, thinking };
    /// Starts the show over the editor window; none when it isn't on screen or Magic isn't shown at work.
    static MagicThinking* begin(Controller& c, const QString& name, const QColor& color);
    static MagicThinking* current();
    Stage stage() const { return stage_; }
    static QString word(Stage s);

    void look();
    /// Analyzing: the clips on the timeline (of these library items, or all when none), those Media items, the viewer.
    void analyze(const std::optional<std::set<Uuid>>& assets);
    /// One library item is being looked at: it's scanned next.
    void focus(const Uuid& asset);
    void think();
    /// The coming edit, sketched as dashed outlines on the timeline.
    void plan(const Project& project);
    /// How long until the outlines drawn so far are fully drawn.
    double secondsUntilDrawn() const;
    /// Ends the show: it fades out where it is, then `done`.
    void end(std::function<void()> done = {});
    /// Hands over to the cursors: the dimming, scans and dots fade; each outline stays until its clip lands.
    void handOver();
    /// Takes the show down at once.
    void cancel();

signals:
    void stageChanged(Stage s);

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    MagicThinking(Controller& c, QWidget* window, const QString& name, const QColor& color);
    ~MagicThinking() override;
    struct Scan { QRectF rect; double start, sweep; double end() const { return start + sweep + 0.55; } };
    struct Ghost { Clip clip; Project project; double born; std::optional<double> landed; };
    double now() const { return clock_.elapsed() / 1000.0; }
    void step();
    void advance(Stage s);
    void addScan(const QRectF& r);
    QRectF viewerRect() const;
    std::optional<QRectF> buttonRect() const;
    std::vector<QRectF> mediaRects(const std::optional<std::set<Uuid>>& assets) const;
    std::optional<QRectF> mediaRect(const Uuid& asset) const;
    std::optional<QRectF> clipRect(const Clip& c, const Project& p) const;
    void setViewerOverlay(bool on);
    Controller& c_;
    QColor color_;
    QString label_;
    Stage stage_ = Stage::looking;
    QElapsedTimer clock_;
    QTimer timer_;
    double last_ = 0, lastEvent_ = 0, lastScan_ = -1, nudged_ = -10, fieldAlpha_ = 0;
    std::optional<double> fadeFrom_;
    bool handedOver_ = false;
    std::function<void()> leaveDone_;
    std::vector<QRectF> targets_, focusQueue_, recent_;
    std::vector<Scan> scans_;
    std::vector<Ghost> ghosts_;
    std::vector<QString> ghostKeys_;
    std::map<Uuid, double> linkBorn_;
};

} // namespace er
