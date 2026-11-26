// TimelinePanel.swift: the timeline's toolbar (the Select and Cut tools, Split, Delete, the in/out readout;
// Strip, Snap, Sticky, Skim, Audio; Zoom to Fit), the track headers (V1/A1, hide, mute, solo, lock, the track
// level pill and its meter, Duck Under; the caption track's header; the add-track buttons), the canvas, and the
// two range bars: under it, a scroll bar whose grips zoom; beside it, one whose grips set the row height.
#pragma once
#include "TimelineCanvas.h"
#include <QAbstractButton>
#include <QLineEdit>

namespace er {

class TimelineToolbar;
class TrackHeaders;

/// A scroll bar thumb with a grip at each end: drag the middle to scroll, drag a grip to zoom.
class RangeBar : public QWidget {
    Q_OBJECT
public:
    RangeBar(Controller& c, bool vertical, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    /// Where the thumb starts and how long it is, along the bar (below the ruler, for the vertical one).
    std::pair<double, double> thumb() const;
    double barLength() const;
    double top() const;
    std::optional<int> part(double p) const;
    /// What a drag started from: every later position is computed from this, never from the previous frame,
    /// so a drag can't drift, flip handles or snap.
    struct BarDrag {
        int part = 1;
        double pps = 0, scrollX = 0, scrollY = 0, rowHeight = 0, length = 0, content = 0, start = 0, mirror = 1;
        std::pair<double, double> anchor;
        double from = 0;
    };
    Controller& c_;
    bool vertical_;
    std::optional<BarDrag> drag_;
    std::optional<int> hot_;
};

class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(Controller& c, QWidget* parent = nullptr);
    TimelineCanvas* canvas() const { return canvas_; }
    /// The toolbar's toggles and tools, after a change made elsewhere (a shortcut, a menu).
    void refreshToggles();
private:
    Controller& c_;
    TimelineToolbar* toolbar_;
    TimelineCanvas* canvas_;
    TrackHeaders* headers_;
};

/// Where a thumb's two edges end up when one grip is dragged (TimelinePanel.swift `gripDrag`). The dragged
/// edge follows the mouse exactly; the other edge mirrors it.
std::pair<double, double> gripDrag(int part, double a0, double b0, double move, double bar, double minLength, double maxLength,
                                   double mirror = 1);

} // namespace er
