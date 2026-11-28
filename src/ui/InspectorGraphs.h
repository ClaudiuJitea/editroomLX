// InspectorView.swift's drawn editors: ColorWheel (the three tint wheels), RampButton (beside Speed and
// Scale), SpeedRampGraph (a clip's speed across its footage), ZoomGraph (the zoom on Scale) and the shape
// buttons along their bottoms (ShapeLabels, EaseGlyph). Maths, sizes, hit areas and wording are the Mac's.
#pragma once
#include "model/ColorGrade.h"
#include "model/Project.h"
#include <QAbstractButton>
#include <QWidget>
#include <functional>
#include <optional>

namespace er {

/// A tint wheel: drag the puck toward a colour to push that tonal range toward it. Double-click resets.
class ColorWheel : public QWidget {
    Q_OBJECT
public:
    /// `level` is the range's brightness, -100...100, on the hairline under the wheel (drag to set, double-click to
    /// reset), shown beside the name once it's moved.
    ColorWheel(const QString& title, WheelPoint point, double level, std::function<void(WheelPoint)> onChange,
               std::function<void(double)> onLevel, std::function<void()> onEnd, QWidget* parent = nullptr);
    void setPoint(const WheelPoint& p) { if (!dragging_) { point_ = p; update(); } }
    void setLevel(double v) { if (!levelDragging_) { level_ = v; update(); } }
    static constexpr int size = 78;
    static constexpr int lineWidth = 66;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    QRectF wheel() const;
    /// The brightness hairline's box (14 px tall, under the wheel).
    QRectF line() const;
    void dragLevel(double x);
    QString title_;
    WheelPoint point_;
    double level_ = 0;
    /// The hairline is being dragged: its number stays up, even passing through zero.
    bool levelDragging_ = false;
    bool lineHover_ = false;
    std::function<void(double)> onLevel_;
    std::function<void(WheelPoint)> onChange_;
    std::function<void()> onEnd_;
    bool dragging_ = false;
    QPointF pressAt_;
    bool moved_ = false;
};

/// Beside the Speed field (and Scale's): turns the clip's speed ramp (or zoom) on or off. A small ramp curve,
/// lit like an active fade curve when on.
class RampButton : public QAbstractButton {
    Q_OBJECT
public:
    RampButton(bool on, const QString& help, std::function<void()> action, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {18, 18}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    bool on_;
};

/// A ramp's or zoom's shape picked by name (In, In/Out, Out), along the bottom of its graph.
class ShapeLabels : public QWidget {
    Q_OBJECT
public:
    struct Option { int value; QString label, help; };
    ShapeLabels(std::vector<Option> options, std::function<void(int)> pick, QWidget* parent = nullptr);
    void setPicked(int v) { picked_ = v; update(); }
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    bool event(QEvent*) override;
private:
    std::vector<QRectF> rects() const;
    std::vector<Option> options_;
    std::function<void(int)> pick_;
    int picked_ = -1;
};

/// Smooth (an S-curve) or linear (a straight line): the zoom's easing, along the bottom right of its graph.
class EaseButtons : public QWidget {
    Q_OBJECT
public:
    EaseButtons(bool smooth, std::function<void(bool)> pick, QWidget* parent = nullptr);
    void setSmooth(bool s) { smooth_ = s; update(); }
    QSize sizeHint() const override { return {45, 18}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    bool event(QEvent*) override;
private:
    bool smooth_;
    std::function<void(bool)> pick_;
};

/// A clip's speed across its footage, under the Speed row. The flat middle is the speed (the same as the
/// slider). An inner dot pulled in from either edge makes a ramp; its outer dot then slides left or right to
/// say where the ramp begins, and up or down for the speed before (or after) it. Across the graph is the
/// clip's footage, which stays put while the clip's length changes.
///
/// Two shapes, picked under the graph: In eases from one speed to another and holds it to the end (its right
/// dot sets both where the ease ends and that speed); In/Out eases into the speed and back out. While a dot is
/// held, its speed shows beside it and the playhead goes to its frame.
class SpeedRampGraph : public QWidget {
    Q_OBJECT
public:
    struct Shape {
        double mid;
        /// Where the ramps begin and end, as fractions across the footage: in-ramp `p1...p2`, out-ramp `p3...p4`.
        double p1 = 0, p2 = 0, p3 = 1, p4 = 1;
        double start, end;
        bool hasIn() const { return p2 - p1 > 0.001; }
        bool hasOut() const { return p4 - p3 > 0.001; }
        /// Eases in and holds: the In shape.
        bool isTwoPoint() const { return hasIn() && !hasOut(); }
    };
    /// In (ease from one speed into another and hold) or In/Out (ease in and back out).
    enum class Mode { rampIn, inOut };

    SpeedRampGraph(const Clip& clip, double fps, QWidget* parent = nullptr);
    void setClip(const Clip& c);
    bool dragging() const { return dragging_.has_value(); }

    std::function<void(bool)> onEditing;
    /// The shape mid-drag, and the fraction across the footage of the dot being dragged (none for the flat middle).
    std::function<void(const Shape&, std::optional<double>)> onChange;
    /// A shape set in one go (the In and In/Out buttons).
    std::function<void(const Shape&)> onSet;

    /// The shape last picked, which new ramps start in (In until another is picked).
    static Mode lastMode();
    /// Timeline time of a point `f` of the way across a clip's footage.
    static double time(double f, const Clip& clip);
    static constexpr int graphHeight = 76;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override { if (!dragging_) unsetCursor(); }
private:
    enum class Part { inStart, inEnd, outStart, outEnd, line };
    Shape shape() const;
    Mode mode(const Shape& s) const { return dragging_ ? draggingMode_ : (s.isTwoPoint() ? Mode::rampIn : Mode::inOut); }
    QRectF graph() const;
    double y(double v, double h) const;
    double speed(double y, double h) const;
    std::vector<std::pair<Part, QPointF>> points(const Shape& s, double w, double h) const;
    QPointF anchor(Part part, const Shape& s, double w, double h) const;
    double fraction(Part part, const Shape& s) const;
    double speedAt(Part part, const Shape& s) const;
    QPainterPath curve(const Shape& s, double w, double h) const;
    std::optional<Part> hit(QPointF p, const Shape& s, double w, double h) const;
    void pick(Mode m, const Shape& s);
    Clip clip_;
    double fps_;
    std::optional<Part> dragging_;
    std::optional<Shape> drawn_;
    Mode draggingMode_ = Mode::rampIn;
    ShapeLabels* modes_;
};

/// The zoom on Scale, drawn like the speed ramp: across is the clip, up is the scale, from the full frame (100%)
/// at the bottom to the zoomed-in Scale. The dots set when each move starts and ends; the top line sets the
/// Scale. Along the bottom: In, In/Out, Out; and smooth or even moves.
class ZoomGraph : public QWidget {
    Q_OBJECT
public:
    ZoomGraph(const Clip& clip, const ZoomMove& zoom, double scale, double fps, QWidget* parent = nullptr);
    void set(const Clip& clip, const ZoomMove& zoom, double scale);
    bool dragging() const { return dragging_.has_value(); }

    std::function<void(bool)> onEditing;
    /// Mid-drag: the zoom, the scale, and the time into the clip of the dot held (none for the top line).
    std::function<void(const ZoomMove&, double, std::optional<double>)> onChange;
    /// A change made in one go (the buttons).
    std::function<void(const ZoomMove&)> onSet;
    static constexpr int graphHeight = 76;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override { if (!dragging_) unsetCursor(); }
private:
    enum class Part { p1, p2, p3, p4, line };
    /// The graph's height in fixed steps: 300%, then 500%, then 1000%.
    static double step(double scale);
    double length() const { return std::max(clip_.duration, 1e-6); }
    double top() const { return dragTop_.value_or(std::max(step(scale_), keptTop_)); }
    QRectF graph() const;
    double y(double v, double h) const;
    double scaleAt(double y, double h) const;
    double x(double t, double w) const;
    double time(Part part, const ZoomMove& z) const;
    std::vector<std::pair<Part, QPointF>> dots(const ZoomMove& z, double s, double w, double h) const;
    std::pair<double, double> lineSpan(const ZoomMove& z) const;
    QPainterPath curve(const ZoomMove& z, double s, double w, double h) const;
    std::optional<Part> hit(QPointF p, const ZoomMove& z, double s, double w, double h) const;
    void pick(ZoomMove::Mode m);
    Clip clip_;
    ZoomMove zoom_;
    double scale_;
    double fps_;
    std::optional<Part> dragging_;
    std::optional<std::pair<ZoomMove, double>> drawn_;
    /// The top of the graph while a drag is under way, so it doesn't rescale under the pointer.
    std::optional<double> dragTop_;
    /// The highest top shown for this clip: the graph steps up when a zoom needs more room, never back down.
    double keptTop_ = 0;
    ShapeLabels* modes_;
    EaseButtons* ease_;
};

} // namespace er
