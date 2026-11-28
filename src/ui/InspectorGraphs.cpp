#include "InspectorGraphs.h"
#include "ExportDialog.h"
#include "Theme.h"
#include <QConicalGradient>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QToolTip>
#include <cmath>

namespace er {

namespace {

/// A dot on a graph: the text colour, 10 px (12 while held), with a 2.5 px ring in the field colour.
void drawDot(QPainter& p, QPointF at, bool held) {
    const double r = held ? 6 : 5;
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::field());
    p.drawEllipse(at, r + 2.5, r + 2.5);
    p.setBrush(Theme::text());
    p.drawEllipse(at, r, r);
}

/// The 100% (or full-frame) reference, the area under the line, and the line.
void drawGraph(QPainter& p, const QPainterPath& line, double baseY, double w) {
    QPen dash(Theme::ink(0.14), 1);
    dash.setDashPattern({2, 3});
    p.setPen(dash);
    p.drawLine(QPointF(0, baseY), QPointF(w, baseY));
    QPainterPath area = line;
    area.lineTo(w, baseY);
    area.lineTo(0, baseY);
    area.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::ink(0.05));
    p.drawPath(area);
    p.setPen(QPen(Theme::text(), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(line);
}

/// The readout beside a held dot (or the top line).
void drawLabel(QPainter& p, const QString& text, QPointF centre) {
    p.setFont(Theme::mono(10.5));
    p.setPen(Theme::text());
    const QFontMetricsF fm(p.font());
    const double w = fm.horizontalAdvance(text);
    p.drawText(QRectF(centre.x() - w / 2 - 2, centre.y() - fm.height() / 2, w + 4, fm.height()), Qt::AlignCenter, text);
}

} // namespace

// MARK: ColorWheel

ColorWheel::ColorWheel(const QString& title, WheelPoint point, double level, std::function<void(WheelPoint)> onChange,
                       std::function<void(double)> onLevel, std::function<void()> onEnd, QWidget* parent)
    : QWidget(parent), title_(title), point_(std::move(point)), level_(level), onLevel_(std::move(onLevel)),
      onChange_(std::move(onChange)), onEnd_(std::move(onEnd)) {
    // The wheel, 4, the hairline (3 above it, 14 tall), 4, the name.
    setFixedHeight(size + 4 + 3 + 14 + 4 + 16);
    setMinimumWidth(size);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setToolTip(QStringLiteral("Drag to tint %1. Double-click to reset.").arg(title.toLower()));
}

QRectF ColorWheel::wheel() const { return QRectF((width() - size) / 2.0, 0, size, size); }
QRectF ColorWheel::line() const { return QRectF((width() - lineWidth) / 2.0, size + 4 + 3, lineWidth, 14); }

void ColorWheel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = wheel();
    const QPointF c = r.center();
    const double s = size;
    const bool active = point_.x != 0 || point_.y != 0;
    const bool lit = (underMouse() && !lineHover_ && !levelDragging_) || dragging_;
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::field());
    p.drawEllipse(r);
    // Hue ring: red at 0°, green at 120°, blue at 240°, going round clockwise (y down), matching the direction
    // the puck tints. Qt's conical gradient runs anticlockwise, so the stops are laid out backwards.
    QConicalGradient g(c, 0);
    const QColor hues[] = {Qt::red, Qt::yellow, Qt::green, Qt::cyan, Qt::blue, QColor(255, 0, 255), Qt::red};
    for (int i = 0; i <= 6; ++i) g.setColorAt(1.0 - i / 6.0, hues[i]);
    p.setOpacity(lit ? 0.95 : 0.6);
    QPainterPath ring;
    ring.addEllipse(r);
    ring.addEllipse(r.adjusted(3, 3, -3, -3));
    p.fillPath(ring, g);
    p.setOpacity(1);
    p.fillRect(QRectF(c.x() - 0.5, c.y() - (s - 14) / 2, 1, s - 14), Theme::ink(0.1));
    p.fillRect(QRectF(c.x() - (s - 14) / 2, c.y() - 0.5, s - 14, 1), Theme::ink(0.1));
    const double pr = dragging_ ? 6.5 : 5.5;
    const QPointF puck(c.x() + point_.x * (s / 2 - 8), c.y() + point_.y * (s / 2 - 8));
    p.setBrush(active ? Theme::accent() : QColor(Qt::white));
    p.setPen(QPen(QColor(0, 0, 0, 128), 1));
    p.drawEllipse(puck, pr - 0.5, pr - 0.5);

    // The range's brightness on a hairline, zero marked in the middle.
    const QRectF L = line();
    const double w = L.width(), mid = L.left() + w / 2, cy = L.center().y();
    const double x = mid + level_ / 100 * w / 2;
    p.setPen(Qt::NoPen);
    p.fillRect(QRectF(L.left(), cy - 0.5, w, 1), Theme::ink(0.14));
    p.fillRect(QRectF(mid - 0.5, cy - 3, 1, 6), Theme::ink(0.3));
    QColor fill = Theme::text();
    fill.setAlphaF(0.75);
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(std::min(x, mid), cy - 1, std::abs(x - mid), 2), 1, 1);
    p.setBrush(Theme::text());
    const double kr = (levelDragging_ || lineHover_ ? 1.2 : 1.0) * 4;
    p.drawEllipse(QPointF(x, cy), kr, kr);

    // The name, with the level beside it once it's moved (and while it's dragged), in its own fixed width wide
    // enough for "-100": the name moves once when it appears, and stays put as the digits change.
    const QFont nameFont = Theme::font(11.5);
    const bool shows = level_ != 0 || levelDragging_;
    const double nameW = QFontMetricsF(nameFont).horizontalAdvance(title_);
    const double total = nameW + (shows ? 4 + 27 : 0);
    const double nx = (width() - total) / 2, ny = L.bottom() + 4;
    p.setFont(nameFont);
    p.setPen(active || level_ != 0 ? Theme::text() : Theme::textDim());
    p.drawText(QRectF(nx, ny, nameW + 1, 16), Qt::AlignLeft | Qt::AlignTop, title_);
    if (shows) {
        const int v = int(level_);
        const QString num = v == 0 ? QStringLiteral("0") : (v > 0 ? QStringLiteral("+") : QString()) + QString::number(v);
        p.setFont(Theme::mono(10.5));
        p.setPen(Theme::textDim());
        p.drawText(QRectF(nx + nameW + 4, ny, 27, 16), Qt::AlignLeft | Qt::AlignVCenter, num);
    }
}

void ColorWheel::dragLevel(double px) {
    const QRectF L = line();
    double value = ((px - L.left()) / L.width() * 2 - 1) * 100;
    value = std::round(std::clamp(value, -100.0, 100.0));
    // A small dead zone makes it easy to land exactly on zero.
    if (std::abs(value) < 3) value = 0;
    level_ = value;
    update();
    onLevel_(value);
}

void ColorWheel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    // The brightness hairline: set where it's pressed, then dragged.
    if (line().adjusted(-4, 0, 4, 0).contains(e->position())) {
        levelDragging_ = true;
        dragLevel(e->position().x());
        return;
    }
    // Only on the wheel itself (its content shape is the circle).
    const QRectF r = wheel();
    if (QLineF(e->position(), r.center()).length() > size / 2.0) return;
    pressAt_ = e->position();
    moved_ = false;
}

void ColorWheel::mouseMoveEvent(QMouseEvent* e) {
    if (levelDragging_) { dragLevel(e->position().x()); return; }
    const bool overLine = line().adjusted(-4, 0, 4, 0).contains(e->position());
    if (overLine != lineHover_) {
        lineHover_ = overLine;
        setToolTip(overLine ? QStringLiteral("Drag to brighten or darken the %1. Double-click to reset.").arg(title_.toLower())
                            : QStringLiteral("Drag to tint %1. Double-click to reset.").arg(title_.toLower()));
        update();
    }
    if (!(e->buttons() & Qt::LeftButton) || pressAt_.isNull()) return;
    // A drag starts after 2 px.
    if (!moved_ && QLineF(e->position(), pressAt_).length() < 2) return;
    moved_ = true;
    dragging_ = true;
    const QPointF c = wheel().center();
    const double s = size;
    double x = (e->position().x() - c.x()) / (s / 2 - 8);
    double y = (e->position().y() - c.y()) / (s / 2 - 8);
    const double m = std::sqrt(x * x + y * y);
    if (m > 1) { x /= m; y /= m; }
    // A small dead zone makes it easy to return to exactly neutral.
    if (m < 0.06) { x = 0; y = 0; }
    point_.x = std::round(x * 100) / 100;
    point_.y = std::round(y * 100) / 100;
    update();
    onChange_(point_);
}

void ColorWheel::mouseReleaseEvent(QMouseEvent*) {
    pressAt_ = QPointF();
    if (levelDragging_) {
        levelDragging_ = false;
        update();
        onEnd_();
        return;
    }
    if (!dragging_) return;
    dragging_ = false;
    update();
    onEnd_();
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent* e) {
    if (line().adjusted(-4, 0, 4, 0).contains(e->position())) {
        levelDragging_ = false;
        level_ = 0;
        update();
        onLevel_(0);
        onEnd_();
        return;
    }
    if (QLineF(e->position(), wheel().center()).length() > size / 2.0) return;
    point_ = WheelPoint();
    update();
    onChange_(WheelPoint());
    onEnd_();
}

// MARK: RampButton

RampButton::RampButton(bool on, const QString& help, std::function<void()> action, QWidget* parent) : QAbstractButton(parent), on_(on) {
    setFixedSize(18, 18);
    setToolTip(help);
    setCursor(Qt::ArrowCursor);
    connect(this, &QAbstractButton::clicked, this, [action = std::move(action)] { action(); });
}

void RampButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg = on_ ? Theme::accent() : Theme::ink(underMouse() ? 0.12 : 0.06);
    if (on_) bg.setAlphaF(0.16);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(rect()), 4, 4);
    // RampGlyph: flat, an S up, flat; 11 × 8.
    const QRectF r(3.5, 5, 11, 8);
    QPainterPath g;
    g.moveTo(r.left(), r.bottom());
    g.lineTo(r.left() + r.width() * 0.22, r.bottom());
    g.cubicTo(QPointF(r.left() + r.width() * 0.42, r.bottom()), QPointF(r.left() + r.width() * 0.4, r.top()),
              QPointF(r.left() + r.width() * 0.6, r.top()));
    g.lineTo(r.right(), r.top());
    p.setPen(QPen(on_ ? Theme::accent() : Theme::textDim(), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(g);
}

// MARK: ShapeLabels

ShapeLabels::ShapeLabels(std::vector<Option> options, std::function<void(int)> pick, QWidget* parent)
    : QWidget(parent), options_(std::move(options)), pick_(std::move(pick)) {
    setFixedHeight(18);
    setFixedWidth(sizeHint().width());
}

std::vector<QRectF> ShapeLabels::rects() const {
    const QFontMetricsF fm(Theme::font(10, 500));
    std::vector<QRectF> out;
    double x = 1.5;
    for (const auto& o : options_) {
        const double w = fm.horizontalAdvance(o.label) + 12;
        out.emplace_back(x, 1.5, w, 15);
        x += w + 1.5;
    }
    return out;
}

QSize ShapeLabels::sizeHint() const {
    const auto r = rects();
    return {int(std::ceil(r.empty() ? 3 : r.back().right() + 1.5)), 18};
}

void ShapeLabels::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::bg());
    p.drawRoundedRect(QRectF(rect()), 4, 4);
    const auto r = rects();
    p.setFont(Theme::font(10, 500));
    for (size_t i = 0; i < options_.size(); ++i) {
        const bool on = options_[i].value == picked_;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::ink(0.14));
            p.drawRoundedRect(r[i], 3, 3);
        }
        QColor c = Theme::text();
        c.setAlphaF(on ? 1 : 0.55);
        p.setPen(c);
        p.drawText(r[i], Qt::AlignCenter, options_[i].label);
    }
}

void ShapeLabels::mousePressEvent(QMouseEvent* e) {
    const auto r = rects();
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i].contains(e->position())) { pick_(options_[i].value); return; }
}

bool ShapeLabels::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const auto r = rects();
        for (size_t i = 0; i < r.size(); ++i)
            if (r[i].contains(he->pos())) { QToolTip::showText(he->globalPos(), options_[i].help, this); return true; }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

// MARK: EaseButtons

EaseButtons::EaseButtons(bool smooth, std::function<void(bool)> pick, QWidget* parent) : QWidget(parent), smooth_(smooth), pick_(std::move(pick)) {
    setFixedSize(45, 18);
}

void EaseButtons::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::bg());
    p.drawRoundedRect(QRectF(rect()), 4, 4);
    for (int i = 0; i < 2; ++i) {
        const bool smooth = i == 0;
        const QRectF b(1.5 + i * 21.5, 1.5, 20, 15);
        const bool on = smooth_ == smooth;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::ink(0.14));
            p.drawRoundedRect(b, 3, 3);
        }
        // EaseGlyph, 9 × 7: an S-curve or a straight line.
        const QRectF g(b.center().x() - 4.5, b.center().y() - 3.5, 9, 7);
        QPainterPath path;
        path.moveTo(g.left(), g.bottom());
        if (smooth) path.cubicTo(QPointF(g.center().x() + g.width() * 0.15, g.bottom()), QPointF(g.center().x() - g.width() * 0.15, g.top()), g.topRight());
        else path.lineTo(g.topRight());
        QColor c = Theme::text();
        c.setAlphaF(on ? 1 : 0.55);
        p.setPen(QPen(c, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
}

void EaseButtons::mousePressEvent(QMouseEvent* e) {
    const bool smooth = e->position().x() < width() / 2.0;
    if (smooth != smooth_) pick_(smooth);
}

bool EaseButtons::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        QToolTip::showText(he->globalPos(), he->pos().x() < width() / 2 ? QStringLiteral("Smooth: eases into and out of each move")
                                                                         : QStringLiteral("Linear: moves at an even pace"), this);
        return true;
    }
    return QWidget::event(e);
}

// MARK: SpeedRampGraph

namespace {
constexpr double rampLo = 0.1, rampHi = 10.0;
constexpr const char* rampModeKey = "speedRampMode";
/// 8 px padding around the graph; the bottom row (shapes and length) under it, 5 px below.
constexpr int pad = 8, rowGap = 5, rowHeight = 18;
} // namespace

SpeedRampGraph::Mode SpeedRampGraph::lastMode() {
    QSettings s;
    const QString m = s.value(QLatin1String(rampModeKey)).toString();
    if (m == QStringLiteral("in")) return Mode::rampIn;
    if (m == QStringLiteral("inOut")) return Mode::inOut;
    // Picked before there were three: 2-point is In, 4-point In/Out.
    return s.value(QStringLiteral("speedRampTwoPoint"), true).toBool() ? Mode::rampIn : Mode::inOut;
}

double SpeedRampGraph::time(double f, const Clip& clip) {
    const double s0 = clip.sourceStart;
    return clip.start + clip.timelineSpan(s0, s0 + clip.sourceLength() * std::clamp(f, 0.0, 1.0));
}

SpeedRampGraph::SpeedRampGraph(const Clip& clip, double fps, QWidget* parent) : QWidget(parent), clip_(clip), fps_(fps) {
    setFixedHeight(pad + graphHeight + rowGap + rowHeight + pad);
    setMouseTracking(true);
    modes_ = new ShapeLabels({{int(Mode::rampIn), QStringLiteral("In"), QStringLiteral("Ease from one speed into another")},
                              {int(Mode::inOut), QStringLiteral("In/Out"), QStringLiteral("Ease into the speed and back out")}},
                             [this](int m) { pick(Mode(m), drawn_.value_or(shape())); }, this);
    modes_->move(pad, pad + graphHeight + rowGap);
    modes_->setPicked(int(mode(shape())));
    setToolTip(QString());
}

void SpeedRampGraph::setClip(const Clip& c) {
    if (dragging_) return;
    clip_ = c;
    modes_->setPicked(int(mode(shape())));
    update();
}

/// The clip's ramp as the graph draws it.
SpeedRampGraph::Shape SpeedRampGraph::shape() const {
    const double s0 = clip_.sourceStart, L = std::max(1e-6, clip_.sourceLength()), s1 = s0 + L;
    auto f = [&](double s) { return std::clamp((s - s0) / L, 0.0, 1.0); };
    Shape out{clip_.speed, 0, 0, 1, 1, clip_.speed, clip_.speed};
    if (clip_.ramp && clip_.isRamped()) {
        const SpeedRamp& r = *clip_.ramp;
        if (r.inTo > r.inFrom && r.inTo > s0 + 1e-6) {
            out.p1 = f(r.inFrom);
            out.p2 = f(r.inTo);
            out.start = clip_.speedAtSource(s0 + out.p1 * L);
        }
        if (r.outTo > r.outFrom && r.outFrom < s1 - 1e-6) {
            out.p3 = std::max(out.p2, f(r.outFrom));
            out.p4 = std::max(out.p3, f(r.outTo));
            out.end = clip_.speedAtSource(s0 + out.p4 * L);
        }
    }
    return out;
}

QRectF SpeedRampGraph::graph() const { return QRectF(pad, pad, width() - pad * 2, graphHeight); }

double SpeedRampGraph::y(double v, double h) const {
    const double n = (std::log(std::clamp(v, rampLo, rampHi)) - std::log(rampLo)) / (std::log(rampHi) - std::log(rampLo));
    return 6 + (h - 12) * (1 - n);
}

double SpeedRampGraph::speed(double yy, double h) const {
    const double n = 1 - (yy - 6) / (h - 12);
    const double v = std::exp(std::log(rampLo) + std::clamp(n, 0.0, 1.0) * (std::log(rampHi) - std::log(rampLo)));
    // A little pull toward 100%.
    return std::abs(std::log(v)) < 0.06 ? 1 : v;
}

std::vector<std::pair<SpeedRampGraph::Part, QPointF>> SpeedRampGraph::points(const Shape& s, double w, double h) const {
    const double midY = y(s.mid, h);
    auto x = [w](double f) { return std::min(std::max(5.0, f * w), w - 5); };
    std::vector<std::pair<Part, QPointF>> out;
    // In has no way back out, so no dot to start one.
    out.push_back({Part::inEnd, QPointF(x(s.p2), midY)});
    if (mode(s) != Mode::rampIn) out.push_back({Part::outStart, QPointF(x(s.p3), midY)});
    if (s.hasIn()) out.push_back({Part::inStart, QPointF(x(s.p1), y(s.start, h))});
    if (s.hasOut()) out.push_back({Part::outEnd, QPointF(x(s.p4), y(s.end, h))});
    return out;
}

QPointF SpeedRampGraph::anchor(Part part, const Shape& s, double w, double h) const {
    auto x = [w](double f) { return std::min(std::max(5.0, f * w), w - 5); };
    switch (part) {
    case Part::inStart: return {x(s.p1), y(s.start, h)};
    case Part::outEnd: return {x(s.p4), y(s.end, h)};
    case Part::inEnd: return {x(s.p2), y(s.mid, h)};
    case Part::outStart: return {x(s.p3), y(s.mid, h)};
    case Part::line: return {(s.p2 + s.p3) / 2 * w, y(s.mid, h)};
    }
    return {};
}

double SpeedRampGraph::fraction(Part part, const Shape& s) const {
    switch (part) {
    case Part::inStart: return s.p1;
    case Part::inEnd: return s.p2;
    case Part::outStart: return s.p3;
    case Part::outEnd: return s.p4;
    case Part::line: return (s.p2 + s.p3) / 2;
    }
    return 0;
}

double SpeedRampGraph::speedAt(Part part, const Shape& s) const {
    switch (part) {
    case Part::inStart: return s.start;
    case Part::outEnd: return s.end;
    default: return s.mid;
    }
}

/// The speed line: flat at the start speed, eased into the middle speed, held, eased out to the end speed.
QPainterPath SpeedRampGraph::curve(const Shape& s, double w, double h) const {
    auto ease = [](double u) { return u * u * (3 - 2 * u); };
    auto blend = [&](double a, double b, double u) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * ease(std::clamp(u, 0.0, 1.0))); };
    auto at = [&](double x) {
        if (s.hasIn() && x < s.p2) return x <= s.p1 ? s.start : blend(s.start, s.mid, (x - s.p1) / (s.p2 - s.p1));
        if (s.hasOut() && x > s.p3) return x >= s.p4 ? s.end : blend(s.mid, s.end, (x - s.p3) / (s.p4 - s.p3));
        return s.mid;
    };
    QPainterPath path;
    const int n = 160;
    for (int i = 0; i <= n; ++i) {
        const double x = double(i) / n;
        const QPointF pt(x * w, y(at(x), h));
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    return path;
}

/// The nearest dot within reach (an inner dot when they sit together), else the flat middle.
std::optional<SpeedRampGraph::Part> SpeedRampGraph::hit(QPointF p, const Shape& s, double w, double h) const {
    std::optional<std::pair<Part, double>> near;
    for (const auto& [part, at] : points(s, w, h)) {
        const double d = std::hypot(p.x() - at.x(), p.y() - at.y());
        if (d >= 10) continue;
        const bool inner = part == Part::inEnd || part == Part::outStart;
        if (near) {
            // Two dots together: the inner one, which makes or shapes the ramp.
            if (std::abs(near->second - d) < 2) { if (inner) near = {part, d}; }
            else if (d < near->second) near = {part, d};
        } else {
            near = {part, d};
        }
    }
    if (near) {
        // Inner dots on top of each other: whichever side the drag is toward.
        if (mode(s) == Mode::inOut && (near->first == Part::inEnd || near->first == Part::outStart)) {
            const double x2 = s.p2 * w, x3 = s.p3 * w;
            if (std::abs(x2 - x3) < 10) return p.x() < (x2 + x3) / 2 ? Part::inEnd : Part::outStart;
        }
        return near->first;
    }
    const double midY = y(s.mid, h);
    if (p.x() > s.p2 * w && p.x() < s.p3 * w && std::abs(p.y() - midY) < 8) return Part::line;
    return std::nullopt;
}

/// Switches shape, keeping as much of the ramp as fits the other one. Remembered for new ramps.
void SpeedRampGraph::pick(Mode m, const Shape& s) {
    QSettings().setValue(QLatin1String(rampModeKey), m == Mode::rampIn ? QStringLiteral("in") : QStringLiteral("inOut"));
    if (m == (s.isTwoPoint() ? Mode::rampIn : Mode::inOut)) return;
    // Just the ease in: the one ease there is, or the way out turned round to come in.
    auto asIn = [](const Shape& s) {
        Shape d = s;
        if (d.hasIn()) { d.p3 = 1; d.p4 = 1; d.end = d.mid; return d; }
        return Shape{s.end, s.p3, s.p4, 1, 1, s.mid, s.end};
    };
    Shape d = asIn(s);
    if (m == Mode::inOut) {
        // Ease back to the starting speed, in the room after the first ease (made if there's little).
        if (d.p2 > 0.6) {
            const double k = 0.6 / d.p2;
            d.p1 *= k;
            d.p2 *= k;
        }
        const double room = 1 - d.p2;
        d.p3 = d.p2 + room * 0.4;
        d.p4 = d.p2 + room * 0.7;
        d.end = d.start;
    }
    if (onSet) onSet(d);
}

void SpeedRampGraph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::field());
    p.drawRoundedRect(QRectF(rect()), 7, 7);
    const Shape s = drawn_.value_or(shape());
    const QRectF g = graph();
    const double w = g.width(), h = g.height();
    p.save();
    p.translate(g.topLeft());
    drawGraph(p, curve(s, w, h), y(1, h), w);
    for (const auto& [part, at] : points(s, w, h)) drawDot(p, at, dragging_ == part);
    if (dragging_) {
        const QPointF a = anchor(*dragging_, s, w, h);
        // The speed beside the dot, on whichever side has room.
        const QString pct = QStringLiteral("%1%").arg(int(std::round(speedAt(*dragging_, s) * 100)));
        drawLabel(p, pct, QPointF(a.x() > w - 44 ? a.x() - 26 : a.x() + 26, a.y() < 16 ? a.y() + 12 : a.y() - 11));
    }
    p.restore();
    // How long the clip plays with its ramp. (A held dot's time shows on the playhead, which follows it.)
    const double rowY = pad + graphHeight + rowGap;
    const QString len = ExportDialog::rangeLength(clip_.duration);
    const QFont mono = Theme::mono(10.5), small = Theme::font(10.5);
    const double lw = QFontMetricsF(mono).horizontalAdvance(len);
    const double right = width() - pad;
    p.setFont(mono);
    p.setPen(Theme::textDim());
    p.drawText(QRectF(right - lw, rowY, lw, rowHeight), Qt::AlignVCenter | Qt::AlignRight, len);
    p.setFont(small);
    p.setPen(Theme::textFaint());
    const double tw = QFontMetricsF(small).horizontalAdvance(QStringLiteral("Length"));
    p.drawText(QRectF(right - lw - 4 - tw, rowY, tw, rowHeight), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Length"));
}

void SpeedRampGraph::mousePressEvent(QMouseEvent* e) {
    const QRectF g = graph();
    if (e->button() != Qt::LeftButton || !g.contains(e->position())) return;
    const Shape s = shape();
    const QPointF p = e->position() - g.topLeft();
    const auto part = hit(p, s, g.width(), g.height());
    if (!part) return;
    draggingMode_ = s.isTwoPoint() ? Mode::rampIn : Mode::inOut;
    dragging_ = part;
    drawn_ = s;
    if (onEditing) onEditing(true);
    mouseMoveEvent(e);
}

void SpeedRampGraph::mouseMoveEvent(QMouseEvent* e) {
    const QRectF g = graph();
    const double w = g.width(), h = g.height();
    const QPointF p = e->position() - g.topLeft();
    if (!dragging_) {
        // The cursor says what a drag would do.
        const Shape s = shape();
        if (!g.contains(e->position())) { unsetCursor(); return; }
        switch (hit(p, s, w, h).value_or(Part(-1))) {
        case Part::inEnd: setCursor(mode(s) == Mode::rampIn ? Qt::OpenHandCursor : Qt::SizeHorCursor); break;
        case Part::outStart: setCursor(Qt::SizeHorCursor); break;
        case Part::inStart: case Part::outEnd: setCursor(Qt::OpenHandCursor); break;
        case Part::line: setCursor(Qt::SizeVerCursor); break;
        default: unsetCursor();
        }
        return;
    }
    Shape d = *drawn_;
    double f = std::clamp(p.x(), 0.0, w) / w;
    if (p.x() < 6) f = 0;
    if (p.x() > w - 6) f = 1;
    switch (*dragging_) {
    case Part::inEnd: {
        const bool had = d.hasIn();
        d.p2 = std::min(std::max(f, d.p1), d.p3);
        if (!d.hasIn()) { d.p1 = d.p2; d.start = d.mid; } else if (!had) { d.p1 = 0; d.start = 1; }
        // 2-point: the right dot is also the speed it eases to.
        if (draggingMode_ == Mode::rampIn) d.mid = speed(p.y(), h);
        break;
    }
    case Part::outStart: {
        const bool had = d.hasOut();
        d.p3 = std::max(std::min(f, d.p4), d.p2);
        if (!d.hasOut()) { d.p4 = d.p3; d.end = d.mid; } else if (!had) { d.p4 = 1; d.end = 1; }
        break;
    }
    case Part::inStart:
        // Up to its inner dot, never onto it (pulling the inner dot back out removes a ramp).
        d.p1 = std::min(f, d.p2 - 0.01);
        d.start = speed(p.y(), h);
        break;
    case Part::outEnd:
        d.p4 = std::max(f, d.p3 + 0.01);
        d.end = speed(p.y(), h);
        break;
    case Part::line:
        d.mid = speed(p.y(), h);
        break;
    }
    // A ramp squeezed to nothing is gone.
    if (!d.hasIn()) { d.p1 = 0; d.p2 = 0; d.start = d.mid; }
    if (!d.hasOut()) { d.p3 = 1; d.p4 = 1; d.end = d.mid; }
    drawn_ = d;
    update();
    if (onChange) onChange(d, *dragging_ == Part::line ? std::nullopt : std::optional<double>(fraction(*dragging_, d)));
}

void SpeedRampGraph::mouseReleaseEvent(QMouseEvent*) {
    if (!dragging_) return;
    dragging_.reset();
    drawn_.reset();
    update();
    if (onEditing) onEditing(false);
}

// MARK: ZoomGraph

ZoomGraph::ZoomGraph(const Clip& clip, const ZoomMove& zoom, double scale, double fps, QWidget* parent)
    : QWidget(parent), clip_(clip), zoom_(zoom), scale_(scale), fps_(fps) {
    setFixedHeight(pad + graphHeight + rowGap + rowHeight + pad);
    setMouseTracking(true);
    modes_ = new ShapeLabels({{int(ZoomMove::Mode::zoomIn), QStringLiteral("In"), QStringLiteral("Zoom in")},
                              {int(ZoomMove::Mode::inOut), QStringLiteral("In/Out"), QStringLiteral("Zoom in, hold, and back out")},
                              {int(ZoomMove::Mode::zoomOut), QStringLiteral("Out"), QStringLiteral("Zoom out")}},
                             [this](int m) { pick(ZoomMove::Mode(m)); }, this);
    modes_->move(pad, pad + graphHeight + rowGap);
    modes_->setPicked(int(zoom.mode));
    ease_ = new EaseButtons(zoom.smooth, [this](bool smooth) {
        ZoomMove d = zoom_;
        d.smooth = smooth;
        if (!(d == zoom_) && onSet) onSet(d);
    }, this);
}

void ZoomGraph::set(const Clip& clip, const ZoomMove& zoom, double scale) {
    if (dragging_) return;
    // Each clip starts its graph at its own height.
    if (clip.id != clip_.id) keptTop_ = 0;
    clip_ = clip;
    zoom_ = zoom;
    scale_ = scale;
    modes_->setPicked(int(zoom.mode));
    ease_->setSmooth(zoom.smooth);
    update();
}

double ZoomGraph::step(double scale) {
    for (double s : {3.0, 5.0}) if (scale < s * 0.97) return s;
    return ClipTransform::scaleMax;
}

QRectF ZoomGraph::graph() const { return QRectF(pad, pad, width() - pad * 2, graphHeight); }

double ZoomGraph::y(double v, double h) const {
    const double t = top();
    const double n = (std::log(std::clamp(v, 1.0, t)) - std::log(1.0)) / (std::log(t) - std::log(1.0));
    return 6 + (h - 12) * (1 - n);
}

double ZoomGraph::scaleAt(double yy, double h) const {
    const double n = 1 - (yy - 6) / (h - 12);
    const double v = std::exp(std::log(1.0) + std::clamp(n, 0.0, 1.0) * (std::log(top()) - std::log(1.0)));
    return std::max(1.05, std::round(v * 100) / 100);
}

double ZoomGraph::x(double t, double w) const { return std::min(std::max(5.0, t / length() * w), w - 5); }

double ZoomGraph::time(Part part, const ZoomMove& z) const {
    switch (part) {
    case Part::p1: return z.p1;
    case Part::p2: return z.p2;
    case Part::p3: return z.p3;
    case Part::p4: return z.p4;
    case Part::line: return z.mode == ZoomMove::Mode::zoomOut ? z.p3 : z.p2;
    }
    return 0;
}

std::vector<std::pair<ZoomGraph::Part, QPointF>> ZoomGraph::dots(const ZoomMove& z, double s, double w, double h) const {
    const double t = y(s, h), b = y(1, h);
    std::vector<std::pair<Part, QPointF>> out;
    if (z.mode != ZoomMove::Mode::zoomOut) {
        out.push_back({Part::p1, QPointF(x(z.p1, w), b)});
        out.push_back({Part::p2, QPointF(x(z.p2, w), t)});
    }
    if (z.mode != ZoomMove::Mode::zoomIn) {
        out.push_back({Part::p3, QPointF(x(z.p3, w), t)});
        out.push_back({Part::p4, QPointF(x(z.p4, w), b)});
    }
    return out;
}

/// Where the top line runs, between the dots at the zoomed-in scale.
std::pair<double, double> ZoomGraph::lineSpan(const ZoomMove& z) const {
    switch (z.mode) {
    case ZoomMove::Mode::zoomIn: return {z.p2, length()};
    case ZoomMove::Mode::zoomOut: return {0, z.p3};
    case ZoomMove::Mode::inOut: return {z.p2, z.p3};
    }
    return {0, 0};
}

QPainterPath ZoomGraph::curve(const ZoomMove& z, double s, double w, double h) const {
    QPainterPath path;
    const int n = 160;
    for (int i = 0; i <= n; ++i) {
        const double u = double(i) / n * length();
        const double v = std::exp(std::log(std::max(s, 1e-3)) * z.amount(u));
        const QPointF pt(u / length() * w, y(v, h));
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    return path;
}

std::optional<ZoomGraph::Part> ZoomGraph::hit(QPointF p, const ZoomMove& z, double s, double w, double h) const {
    std::optional<std::pair<Part, double>> near;
    for (const auto& [part, at] : dots(z, s, w, h)) {
        const double d = std::hypot(p.x() - at.x(), p.y() - at.y());
        if (d < 10 && d < (near ? near->second : INFINITY)) near = {part, d};
    }
    if (near) return near->first;
    const auto span = lineSpan(z);
    if (p.x() > x(span.first, w) && p.x() < x(span.second, w) && std::abs(p.y() - y(s, h)) < 8) return Part::line;
    return std::nullopt;
}

/// Another mode, starting fresh for it (keeping smooth or linear). Remembered for new zooms.
void ZoomGraph::pick(ZoomMove::Mode m) {
    if (m == zoom_.mode) return;
    ZoomMove d = ZoomMove::starter(m, length());
    d.smooth = zoom_.smooth;
    if (onSet) onSet(d);
}

void ZoomGraph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::field());
    p.drawRoundedRect(QRectF(rect()), 7, 7);
    const ZoomMove z = drawn_ ? drawn_->first : zoom_;
    const double s = drawn_ ? drawn_->second : scale_;
    const QRectF g = graph();
    const double w = g.width(), h = g.height();
    p.save();
    p.translate(g.topLeft());
    drawGraph(p, curve(z, s, w, h), y(1, h), w);
    for (const auto& [part, at] : dots(z, s, w, h)) drawDot(p, at, dragging_ == part);
    if (dragging_ == Part::line) {
        const auto span = lineSpan(z);
        const QPointF a((x(span.first, w) + x(span.second, w)) / 2, y(s, h));
        drawLabel(p, QStringLiteral("%1%").arg(int(std::round(s * 100))), QPointF(a.x(), a.y() < 16 ? a.y() + 12 : a.y() - 11));
    }
    p.restore();
    ease_->move(width() - pad - ease_->width(), pad + graphHeight + rowGap);
}

void ZoomGraph::mousePressEvent(QMouseEvent* e) {
    const QRectF g = graph();
    if (e->button() != Qt::LeftButton || !g.contains(e->position())) return;
    const QPointF p = e->position() - g.topLeft();
    const auto part = hit(p, zoom_, scale_, g.width(), g.height());
    if (!part) return;
    dragTop_ = top();
    dragging_ = part;
    drawn_ = {zoom_, scale_};
    if (onEditing) onEditing(true);
    mouseMoveEvent(e);
}

void ZoomGraph::mouseMoveEvent(QMouseEvent* e) {
    const QRectF g = graph();
    const double w = g.width(), h = g.height();
    const QPointF p = e->position() - g.topLeft();
    if (!dragging_) {
        if (!g.contains(e->position())) { unsetCursor(); return; }
        const auto part = hit(p, zoom_, scale_, w, h);
        if (!part) unsetCursor();
        else if (*part == Part::line) setCursor(Qt::SizeVerCursor);
        else setCursor(Qt::SizeHorCursor);
        return;
    }
    auto [d, ds] = *drawn_;
    double t = std::clamp(p.x(), 0.0, w) / w * length();
    if (p.x() < 6) t = 0;
    if (p.x() > w - 6) t = length();
    t = std::round(t * fps_) / fps_;
    const double gap = 1 / fps_;
    switch (*dragging_) {
    case Part::p1: d.p1 = std::min(t, d.p2 - gap); break;
    case Part::p2: d.p2 = std::min(std::max(t, d.p1 + gap), d.mode == ZoomMove::Mode::inOut ? d.p3 : length()); break;
    case Part::p3: d.p3 = std::max(std::min(t, d.p4 - gap), d.mode == ZoomMove::Mode::inOut ? d.p2 : 0.0); break;
    case Part::p4: d.p4 = std::max(t, d.p3 + gap); break;
    case Part::line: ds = scaleAt(p.y(), h); break;
    }
    d.p1 = std::max(0.0, d.p1);
    d.p4 = std::min(length(), d.p4);
    drawn_ = {d, ds};
    update();
    if (onChange) onChange(d, ds, *dragging_ == Part::line ? std::nullopt : std::optional<double>(time(*dragging_, d)));
}

void ZoomGraph::mouseReleaseEvent(QMouseEvent*) {
    if (!dragging_) return;
    if (drawn_) keptTop_ = std::max({keptTop_, step(drawn_->second), dragTop_.value_or(0)});
    dragging_.reset();
    drawn_.reset();
    dragTop_.reset();
    update();
    if (onEditing) onEditing(false);
}

} // namespace er
