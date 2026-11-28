#include "TimelineCanvas.h"
#include "MarkerPanel.h"
#include "Symbols.h"
#include "Theme.h"
#include "model/ColorGrade.h"
#include "model/Timecode.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPointer>
#include <QPainterPath>
#include <QFileInfo>
#include <QUrl>
#include <chrono>
#include <cmath>

namespace er {

namespace {
const char* kAssetMime = "application/x-editroom-asset";

QColor mix(const QColor& a, const QColor& b, double f) {
    return QColor::fromRgbF(float(a.redF() * (1 - f) + b.redF() * f), float(a.greenF() * (1 - f) + b.greenF() * f),
                            float(a.blueF() * (1 - f) + b.blueF() * f), float(a.alphaF() * (1 - f) + b.alphaF() * f));
}
QColor white(double alpha) { return QColor::fromRgbF(1, 1, 1, float(alpha)); }
QColor black(double alpha) { return QColor::fromRgbF(0, 0, 0, float(alpha)); }

QFont labelFont() { return Theme::font(10.5, 600); }
QFont rulerFont() { return Theme::mono(10, 400); }
QFont bubbleFont() { return Theme::mono(10.5, 600); }
QFont captionFont() { return Theme::font(10.5, 500); }
QFont hintFont() { return Theme::font(11.5); }
QFont readoutFont() { return Theme::mono(10, 500); }
QFont markerFont() { return Theme::mono(10, 500); }

/// Text with the Mac's soft shadow under it, so names read where the waveform runs up behind them.
void shadowText(QPainter& p, QPointF at, const QString& s, const QFont& f) {
    p.setFont(f);
    const QFontMetricsF fm(f);
    const QPointF base(at.x(), at.y() + fm.ascent());
    p.setPen(black(0.35));
    for (const QPointF d : {QPointF(0, 1), QPointF(1, 0.5), QPointF(-1, 0.5), QPointF(0, 1.5)}) p.drawText(base + d, s);
    p.setPen(white(0.95));
    p.drawText(base, s);
}

/// The playhead lives in its own widget so moving it never redraws the clips.
class PlayheadView : public QWidget {
public:
    static constexpr int width = 16;
    explicit PlayheadView(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        resize(width, 100);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double mid = width / 2.0;
        const double l = mid - 5.5, r = mid + 5.5;
        const double top = 5, shoulder = 13, tip = 19, rad = 3;
        // One closed shape: rounded cap narrowing to a point, with the stem running straight out of the tip.
        QPainterPath path;
        path.moveTo(l, top + rad);
        path.arcTo(QRectF(l, top, rad * 2, rad * 2), 180, -90);
        path.lineTo(r - rad, top);
        path.arcTo(QRectF(r - rad * 2, top, rad * 2, rad * 2), 90, -90);
        path.lineTo(r, shoulder);
        path.lineTo(mid + 0.5, tip);
        path.lineTo(mid + 0.5, height());
        path.lineTo(mid - 0.5, height());
        path.lineTo(mid - 0.5, tip);
        path.lineTo(l, shoulder);
        path.closeSubpath();
        p.fillPath(path, Theme::accent());
    }
};

QCursor cutCursor() {
    static const QCursor c = [] {
        QPixmap pm(48, 48);
        pm.setDevicePixelRatio(2);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        // White scissors with a dark halo, as the Mac's.
        for (const QPointF d : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)})
            Symbols::draw(p, QStringLiteral("scissors"), QPointF(12, 12) + d, 15, black(0.6), 600);
        Symbols::draw(p, QStringLiteral("scissors"), QPointF(12, 12), 15, Qt::white, 600);
        p.end();
        return QCursor(pm, 12, 12);
    }();
    return c;
}
} // namespace

namespace {
QPointer<TimelineCanvas> currentCanvas;
}

TimelineCanvas::TimelineCanvas(Controller& c, QWidget* parent)
    : QWidget(parent), c_(c), layout_(Project(), 62) {
    currentCanvas = this;
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(120);
    setAttribute(Qt::WA_OpaquePaintEvent);
    playhead_ = new PlayheadView(this);
    EditorModel& m = c_.model();
    connect(&m, &EditorModel::changed, this, qOverload<>(&QWidget::update));
    connect(&m, &EditorModel::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(&m, &EditorModel::viewChanged, this, [this] { placePlayhead(); update(); });
    connect(&m, &EditorModel::editingMarkerChanged, this, &TimelineCanvas::showMarkerPanel);
    connect(&m, &EditorModel::playheadChanged, this, [this](double t) {
        // While playing, the playhead is kept in view: the page turns when it reaches the edge.
        const double px = x(t);
        if (c_.model().isPlaying() && (px > width() - 28 || px < 0)) c_.model().setView(std::nullopt, t * pps() - 48);
        placePlayhead();
        // A graph dot dragged in the inspector moves the playhead; its time tag goes with it.
        if (c_.model().showPlayheadTime() || scrubReadout_ || c_.model().skimTime()) update();
    });
    connect(&c_.thumbnails(), &Thumbnails::ready, this, qOverload<>(&QWidget::update));
    connect(&c_.sound(), &SoundStore::waveformReady, this, qOverload<>(&QWidget::update));
    connect(&c_.media(), &MediaStore::visualChanged, this, qOverload<>(&QWidget::update));
    connect(&c_, &Controller::importChanged, this, qOverload<>(&QWidget::update));
    autoscroll_.setInterval(1000 / 60);
    connect(&autoscroll_, &QTimer::timeout, this, [this] {
        c_.model().setView(std::nullopt, view().scrollX + autoscrollVelocity_, view().scrollY);
        handleDrag(lastDragPoint_);
    });
}

TimelineCanvas::~TimelineCanvas() = default;

void TimelineCanvas::resizeEvent(QResizeEvent*) {
    c_.model().setViewSize(width(), height());
    playhead_->resize(PlayheadView::width, height());
    // In place now: a timeline made afresh otherwise showed its playhead at the far left for a frame.
    placePlayhead();
}

void TimelineCanvas::placePlayhead() {
    const double px = x(c_.model().playhead());
    const bool hidden = px < -PlayheadView::width || px > width() + PlayheadView::width;
    playhead_->setVisible(!hidden);
    playhead_->move(int(std::round(px - PlayheadView::width / 2.0)), 0);
    playhead_->raise();
}

// MARK: Geometry

TimelineCanvas* TimelineCanvas::current() { return currentCanvas.data(); }

std::optional<QRectF> TimelineCanvas::rect(const Clip& c) const {
    auto band = layout_.band(c.trackID);
    if (!band) return std::nullopt;
    return QRectF(x(c.start), TimelineLayout::ruler + band->first - view().scrollY, std::max(c.duration * pps(), 1.0), band->second);
}

std::optional<QRectF> TimelineCanvas::rect(const Clip& c, const Project& in) const {
    const TimelineLayout layout(in, view().rowHeight);
    auto band = layout.band(c.trackID);
    if (!band) return std::nullopt;
    return QRectF(x(c.start), TimelineLayout::ruler + band->first - view().scrollY, std::max(c.duration * pps(), 1.0), band->second);
}

/// A caption's block on the caption track.
std::optional<QRectF> TimelineCanvas::captionRect(const CaptionBlock& b) const {
    if (!layout_.captionY) return std::nullopt;
    return QRectF(x(b.start), TimelineLayout::ruler + *layout_.captionY - view().scrollY, std::max((b.end - b.start) * pps(), 2.0),
                  TimelineLayout::captionRow);
}

bool TimelineCanvas::inCaptionRow(QPointF p) const {
    if (!layout_.captionY || p.y() < TimelineLayout::ruler) return false;
    const double y = p.y() - TimelineLayout::ruler + view().scrollY;
    return y >= *layout_.captionY && y < *layout_.captionY + TimelineLayout::captionRow;
}

std::optional<CaptionBlock> TimelineCanvas::captionHit(QPointF p) const {
    if (!inCaptionRow(p)) return std::nullopt;
    const auto blocks = c_.model().captionBlocks();
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
        if (auto r = captionRect(*it); r && r->adjusted(-2, 0, 2, 0).contains(p)) return *it;
    return std::nullopt;
}

TimelineCanvas::Lane TimelineCanvas::lane(double y) const {
    const double cy = y - TimelineLayout::ruler + view().scrollY;
    if (cy < layout_.videoZoneY + TimelineLayout::zone) return {Lane::Kind::newVideo, {}, 0};
    if (cy >= layout_.audioZoneY) return {Lane::Kind::newAudio, {}, 0};
    for (const auto& row : layout_.rows)
        if (cy >= row.y && cy < row.y + row.height + TimelineLayout::gap) return {Lane::Kind::track, row.track, row.index};
    return {};
}

// MARK: Snapping

/// Ctrl turns snapping the other way while it's held (⌘ on the Mac).
bool TimelineCanvas::snappingActive() const {
    return c_.model().snapping != bool((modifiers_ | QApplication::keyboardModifiers()) & Qt::ControlModifier);
}

std::optional<std::pair<double, double>> TimelineCanvas::snap(const std::vector<double>& candidates, const std::vector<double>& targets, double pixels) const {
    if (!snappingActive()) return std::nullopt;
    const double threshold = pixels / pps();
    std::optional<std::pair<double, double>> best;
    for (double cand : candidates) {
        for (double t : targets) {
            const double d = t - cand;
            if (std::abs(d) <= threshold && (!best || std::abs(d) < std::abs(best->first))) best = std::make_pair(d, t);
        }
    }
    return best;
}

// MARK: Hit testing

double TimelineCanvas::fadeHandleX(const Clip& c, const QRectF& r, bool fadeIn) const {
    const double raw = fadeIn ? r.left() + c.fadeIn * pps() : r.right() - c.fadeOut * pps();
    return std::min(std::max(raw, r.left() + 6), r.right() - 6);
}

double TimelineCanvas::gainNorm(double db) {
    return db >= 0 ? 0.3 - db / 12 * 0.2 : 0.3 + -db / 60 * 0.62;
}

double TimelineCanvas::gainDB(double norm) {
    const double n = std::min(std::max(norm, 0.1), 0.92);
    const double db = n <= 0.3 ? (0.3 - n) / 0.2 * 12 : -((n - 0.3) / 0.62) * 60;
    return std::round(db * 10) / 10;
}

std::optional<TimelineCanvas::Hit> TimelineCanvas::hit(QPointF p) const {
    if (p.y() < TimelineLayout::ruler) return std::nullopt;
    const Project& proj = c_.model().project();
    for (const Clip& c : proj.clips) {
        if (proj.isLocked(c.trackID)) continue;
        auto rr = rect(c);
        if (!rr || !rr->contains(p)) continue;
        const QRectF r = *rr;
        // Fade dots only for fades set (in the inspector): an unset fade has no dot to catch the mouse, so a
        // clip's edges always trim.
        const bool showHandles = r.width() > 36 && (c.fadeIn > 0 || c.fadeOut > 0);
        if (showHandles && p.y() < r.top() + 17) {
            const double hi = fadeHandleX(c, r, true), ho = fadeHandleX(c, r, false);
            const double di = c.fadeIn > 0 ? std::abs(p.x() - hi) : INFINITY, dout = c.fadeOut > 0 ? std::abs(p.x() - ho) : INFINITY;
            if (std::min(di, dout) <= 7) {
                bool takeIn = di <= dout;
                // Overlapping dots: the one with the longer fade (the one dragged there) is on top and takes the
                // click; with equal fades, the side clicked decides.
                if (std::abs(hi - ho) < 8 && c.fadeIn > 0 && c.fadeOut > 0) takeIn = c.fadeIn != c.fadeOut ? c.fadeIn > c.fadeOut : p.x() < hi;
                return Hit{c, takeIn ? Zone::fadeIn : Zone::fadeOut, r};
            }
        }
        const double edge = std::min(7.0, std::max(2.0, r.width() / 4));
        if (p.x() - r.left() <= edge) return Hit{c, Zone::left, r};
        if (r.right() - p.x() <= edge) return Hit{c, Zone::right, r};
        if (c.kind == ClipKind::audio && std::abs(p.y() - (r.top() + gainNorm(c.gainDB) * r.height())) <= 3.5) return Hit{c, Zone::gain, r};
        return Hit{c, Zone::body, r};
    }
    return std::nullopt;
}

// MARK: Mouse

void TimelineCanvas::clearHover() {
    if (c_.model().skimTime()) c_.model().skim(std::nullopt);
    if (hover_ || cutHover_) {
        hover_.reset();
        cutHover_.reset();
        update();
    }
    unsetCursor();
}

void TimelineCanvas::updateHover(QPointF p) {
    EditorModel& m = c_.model();
    std::optional<std::pair<Uuid, Zone>> newHover;
    std::optional<std::pair<Uuid, double>> newCut;
    QCursor cursor = Qt::ArrowCursor;
    if (p.y() < TimelineLayout::ruler) {
        cursor = Qt::ArrowCursor;
    } else if (m.tool == Tool::blade) {
        cursor = cutCursor();
        if (auto h = hit(p)) {
            double t = time(p.x());
            auto others = m.project().snapTargets({h->clip.id});
            others.push_back(m.playhead());
            if (auto s = snap({t}, others, 6)) t += s->first; else t = m.project().snapFrame(t);
            const double minDur = 0.5 / m.project().fps;
            if (t > h->clip.start + minDur && t < h->clip.end() - minDur) {
                newCut = std::make_pair(h->clip.id, t);
                newHover = std::make_pair(h->clip.id, Zone::body);
            }
        }
    } else if (auto h = hit(p)) {
        newHover = std::make_pair(h->clip.id, h->zone);
        switch (h->zone) {
        case Zone::left: case Zone::right: cursor = Qt::SizeHorCursor; break;
        case Zone::fadeIn: case Zone::fadeOut: cursor = Qt::PointingHandCursor; break;
        case Zone::gain: cursor = Qt::SizeVerCursor; break;
        case Zone::body: cursor = Qt::ArrowCursor; break;
        }
    }
    const bool changed = newHover != hover_ || newCut != cutHover_;
    hover_ = newHover;
    cutHover_ = newCut;
    if (changed) update();
    setCursor(cursor);
}

void TimelineCanvas::leaveEvent(QEvent*) {
    if (c_.model().skimTime()) {
        c_.model().skim(std::nullopt);
        update();
    }
    if (hover_ || cutHover_) {
        hover_.reset();
        cutHover_.reset();
        update();
    }
}

void TimelineCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) press(e, 1);
}

void TimelineCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) press(e, 2);
}

void TimelineCanvas::press(QMouseEvent* e, int clickCount) {
    EditorModel& m = c_.model();
    // Working on the timeline: Delete is for clips again, not Media panel items.
    m.mediaSelection.clear();
    m.folderSelection.clear();
    m.skim(std::nullopt);
    setFocus();
    const QPointF p = e->position();
    lastDragPoint_ = p;
    modifiers_ = e->modifiers();
    const bool extend = e->modifiers() & Qt::ShiftModifier;

    if (p.y() < TimelineLayout::ruler) {
        // The playhead sits on top of markers, so near it a click takes the playhead; a marker under it can still
        // be clicked by its name.
        const bool onPlayhead = std::abs(p.x() - x(m.playhead())) <= 6;
        if (!onPlayhead) {
            if (auto id = markerHit(p)) {
                drag_ = Drag();
                drag_.kind = Drag::Kind::marker;
                drag_.id = *id;
                drag_.startX = p.x();
                return;
            }
            if (auto isIn = rangeEdgeHit(p)) {
                drag_ = Drag();
                drag_.kind = Drag::Kind::rangeEdge;
                drag_.isIn = *isIn;
                return;
            }
        }
        drag_ = Drag();
        drag_.kind = Drag::Kind::scrub;
        scrub(p);
        return;
    }

    // The caption track: pick captions (Shift adds); a double-click goes to one to retype it.
    if (inCaptionRow(p)) {
        drag_ = Drag();
        const QPointF content(p.x() + view().scrollX, p.y() + view().scrollY);
        auto b = captionHit(p);
        if (!b) {
            if (!extend) m.selectCaptions({});
            drag_.kind = Drag::Kind::captionMarquee;
            drag_.origin = content;
            drag_.captionBase = m.captionSelection();
            return;
        }
        if (extend) {
            auto picked = m.captionSelection();
            if (picked.count(b->id())) picked.erase(b->id()); else picked.insert(b->id());
            m.selectCaptions(picked);
        } else {
            m.selectCaptions({b->id()});
        }
        if (clickCount == 2) {
            m.selectCaptions({b->id()});
            m.seek((b->start + b->end) / 2);
            m.requestCaptionEdit(b->id());
        } else {
            // Captions don't move, so a drag from one boxes in more.
            drag_.kind = Drag::Kind::captionMarquee;
            drag_.origin = content;
            if (extend) drag_.captionBase = m.captionSelection();
        }
        update();
        return;
    }

    if (m.tool == Tool::blade) {
        updateHover(p);
        if (cutHover_) {
            m.split(cutHover_->first, cutHover_->second);
            cutHover_.reset();
            update();
        }
        return;
    }

    auto h = hit(p);
    if (!h) {
        if (!extend) { m.setSelection({}); m.releaseCaptions(); }
        drag_ = Drag();
        drag_.kind = Drag::Kind::marquee;
        drag_.origin = QPointF(p.x() + view().scrollX, p.y() + view().scrollY);
        drag_.base = m.selection();
        if (extend) drag_.captionBase = m.captionSelection();
        return;
    }

    const IdSet group = m.project().unlocked(m.project().linked({h->clip.id}));
    drag_ = Drag();
    switch (h->zone) {
    case Zone::body:
        if (extend) {
            IdSet sel = m.selection();
            if (sel.count(h->clip.id)) for (const auto& id : group) sel.erase(id);
            else sel.insert(group.begin(), group.end());
            m.setSelection(sel);
            return;
        }
        if (!m.selection().count(h->clip.id)) m.setSelection(group);
        drag_.kind = Drag::Kind::pendingMove;
        drag_.grabbed = h->clip;
        drag_.point = p;
        drag_.time = time(p.x());
        break;
    case Zone::left:
    case Zone::right: {
        if (!m.selection().count(h->clip.id)) m.setSelection(group);
        // Every selected clip's same edge moves together (any track, lined up or not); with nothing else
        // selected, that's this clip and what's linked to it.
        IdSet both = m.selection();
        both.insert(group.begin(), group.end());
        const IdSet trimmed = m.project().unlocked(m.project().linked(both));
        drag_.kind = Drag::Kind::trim;
        drag_.grabbed = h->clip;
        drag_.left = h->zone == Zone::left;
        drag_.time = time(p.x());
        for (const auto& c : m.project().clips) if (trimmed.count(c.id)) drag_.originals.push_back(c);
        break;
    }
    case Zone::fadeIn:
    case Zone::fadeOut:
        drag_.kind = Drag::Kind::fade;
        drag_.id = h->clip.id;
        drag_.fadeIn = h->zone == Zone::fadeIn;
        break;
    case Zone::gain:
        if (clickCount == 2) {
            const Uuid id = h->clip.id;
            m.mutate([&](Project& proj) { if (auto i = proj.index(id)) proj.clips[*i].gainDB = 0; });
            return;
        }
        drag_.kind = Drag::Kind::gain;
        drag_.id = h->clip.id;
        drag_.startY = p.y();
        drag_.startNorm = gainNorm(h->clip.gainDB);
        drag_.height = h->rect.height();
        break;
    }
}

void TimelineCanvas::mouseMoveEvent(QMouseEvent* e) {
    const QPointF p = e->position();
    modifiers_ = e->modifiers();
    if (!(e->buttons() & Qt::LeftButton) || drag_.kind == Drag::Kind::none) {
        updateHover(p);
        EditorModel& m = c_.model();
        if (m.skimming() && !m.isPlaying()) {
            m.skim(time(p.x()));
            update();
        }
        return;
    }
    lastDragPoint_ = p;
    handleDrag(p);
    switch (drag_.kind) {
    case Drag::Kind::scrub: case Drag::Kind::move: case Drag::Kind::trim: case Drag::Kind::marquee: case Drag::Kind::captionMarquee:
        updateAutoscroll(p);
        break;
    default:
        break;
    }
}

void TimelineCanvas::handleDrag(QPointF p) {
    EditorModel& m = c_.model();
    switch (drag_.kind) {
    case Drag::Kind::none:
        break;
    case Drag::Kind::scrub:
        scrub(p);
        break;
    case Drag::Kind::pendingMove: {
        if (!(std::abs(p.x() - drag_.point.x()) > 3 || std::abs(p.y() - drag_.point.y()) > 3)) return;
        const IdSet ids = m.project().unlocked(m.project().linked(m.selection()));
        drag_.originals.clear();
        for (const auto& c : m.project().clips) if (ids.count(c.id)) drag_.originals.push_back(c);
        moveTrackDelta_ = 0;
        drag_.kind = Drag::Kind::move;
        setCursor(Qt::ClosedHandCursor);
        updateMove(drag_.grabbed, drag_.time, drag_.originals, p);
        break;
    }
    case Drag::Kind::move:
        updateMove(drag_.grabbed, drag_.time, drag_.originals, p);
        break;
    case Drag::Kind::trim:
        updateTrim(drag_.grabbed, drag_.left, drag_.time, drag_.originals, p);
        break;
    case Drag::Kind::fade: {
        const double t = time(p.x());
        const Uuid id = drag_.id;
        const bool fadeIn = drag_.fadeIn;
        m.live([&](Project& proj) {
            auto i = proj.index(id);
            if (!i) return;
            Clip& c = proj.clips[*i];
            if (fadeIn) c.fadeIn = std::min(std::max(0.0, proj.snapFrame(t - c.start)), c.duration - c.fadeOut);
            else c.fadeOut = std::min(std::max(0.0, proj.snapFrame(c.end() - t)), c.duration - c.fadeIn);
        }, false);
        if (const Clip* c = m.project().clip(id))
            bubble_ = std::make_pair(QString::asprintf("%.2fs", fadeIn ? c->fadeIn : c->fadeOut), QPointF(p.x(), p.y() - 22));
        update();
        break;
    }
    case Drag::Kind::gain: {
        const double db = gainDB(drag_.startNorm + (p.y() - drag_.startY) / drag_.height);
        const double value = db <= -59.5 ? -60 : db;
        const Uuid id = drag_.id;
        m.live([&](Project& proj) { if (auto i = proj.index(id)) proj.clips[*i].gainDB = value; }, false);
        bubble_ = std::make_pair(formatDB(value), QPointF(p.x() + 12, p.y() - 22));
        update();
        break;
    }
    case Drag::Kind::marker: {
        if (!drag_.moved && std::abs(p.x() - drag_.startX) < 3) return;
        drag_.moved = true;
        double t = std::max(0.0, time(p.x()));
        double was = -1;
        for (const auto& mk : m.project().markerList()) if (mk.id == drag_.id) was = mk.time;
        std::vector<double> others;
        for (double s : m.project().snapTargets({})) if (std::abs(s - was) > 1e-9) others.push_back(s);
        others.push_back(m.playhead());
        if (auto s = snap({t}, others, 6)) t += s->first;
        const double snapped = m.project().snapFrame(t);
        m.updateMarkerLive(drag_.id, [snapped](Marker& mk) { mk.time = snapped; });
        scrubReadout_ = snapped;
        update();
        break;
    }
    case Drag::Kind::rangeEdge: {
        double t = std::max(0.0, time(p.x()));
        auto targets = m.project().snapTargets({});
        targets.push_back(m.playhead());
        if (auto s = snap({t}, targets, 6)) t += s->first;
        const double snapped = m.project().snapFrame(t);
        m.setRangeEdgeLive(drag_.isIn, snapped);
        scrubReadout_ = snapped;
        update();
        break;
    }
    case Drag::Kind::marquee: {
        const QPointF current(p.x() + view().scrollX, p.y() + view().scrollY);
        const QRectF content(QPointF(std::min(drag_.origin.x(), current.x()), std::min(drag_.origin.y(), current.y())),
                             QPointF(std::max(drag_.origin.x(), current.x()), std::max(drag_.origin.y(), current.y())));
        const QRectF r = content.translated(-view().scrollX, -view().scrollY);
        marquee_ = r;
        IdSet ids = drag_.base;
        for (const auto& c : m.project().clips) {
            if (m.project().isLocked(c.trackID)) continue;
            if (auto cr = rect(c); cr && cr->intersects(r)) ids.insert(c.id);
        }
        ids = m.project().unlocked(m.project().linked(ids));
        // A box drawn up over the caption track, touching no clips, picks the captions in it.
        std::set<QString> boxed;
        for (const auto& b : m.captionBlocks()) if (auto br = captionRect(b); br && br->intersects(r)) boxed.insert(b.id());
        std::set<QString> captions = boxed;
        if (!boxed.empty()) captions.insert(drag_.captionBase.begin(), drag_.captionBase.end());
        if (ids == drag_.base && !captions.empty()) {
            if (captions != m.captionSelection()) m.selectCaptions(captions);
        } else {
            if (!m.captionSelection().empty() && ids != drag_.base) m.releaseCaptions();
            if (ids != m.selection()) m.setSelection(ids);
        }
        update();
        break;
    }
    case Drag::Kind::captionMarquee: {
        const QPointF current(p.x() + view().scrollX, p.y() + view().scrollY);
        const QRectF r = QRectF(QPointF(std::min(drag_.origin.x(), current.x()), std::min(drag_.origin.y(), current.y())),
                                QPointF(std::max(drag_.origin.x(), current.x()), std::max(drag_.origin.y(), current.y())))
                             .translated(-view().scrollX, -view().scrollY);
        marquee_ = r;
        std::set<QString> ids = drag_.captionBase;
        for (const auto& b : m.captionBlocks()) if (auto br = captionRect(b); br && br->intersects(r)) ids.insert(b.id());
        if (ids != m.captionSelection()) m.selectCaptions(ids);
        update();
        break;
    }
    }
}

void TimelineCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    EditorModel& m = c_.model();
    stopAutoscroll();
    const Drag finished = drag_;
    drag_ = Drag();
    // A trim showed its edge's frame in the viewer: back to the edit.
    if (finished.kind == Drag::Kind::trim) c_.sourcePreview().clear();
    switch (finished.kind) {
    case Drag::Kind::move:
        if (duplicating_) {
            // Alt-drag: the clips stay where they were and copies land where they were dropped.
            std::vector<Clip> moved;
            for (const auto& [id, c] : overrides_) moved.push_back(c);
            overrides_.clear();
            duplicating_ = false;
            if (!moved.empty()) {
                double t0 = INFINITY;
                for (const auto& c : moved) t0 = std::min(t0, c.start);
                std::vector<Clip> shifted = moved;
                for (auto& c : shifted) c.start -= t0;
                auto copies = m.placed(shifted, t0);
                for (size_t i = 0; i < copies.size() && i < moved.size(); ++i) copies[i].trackID = moved[i].trackID;
                m.insert(copies);
            }
            break;
        }
        [[fallthrough]];
    case Drag::Kind::trim: {
        const auto moved = overrides_;
        overrides_.clear();
        if (!moved.empty()) {
            IdSet keys;
            for (const auto& [id, c] : moved) keys.insert(id);
            m.mutate([&](Project& proj) {
                for (const auto& [id, c] : moved) if (auto i = proj.index(id)) proj.clips[*i] = c;
                proj.materializeTokens();
                proj.resolveOverlaps(keys);
            });
        }
        break;
    }
    case Drag::Kind::marker:
        scrubReadout_.reset();
        if (finished.moved) m.commitLive(); else m.setEditingMarker(finished.id);
        break;
    case Drag::Kind::rangeEdge:
        scrubReadout_.reset();
        m.commitLive();
        break;
    case Drag::Kind::fade:
    case Drag::Kind::gain:
        m.commitLive();
        break;
    default:
        break;
    }
    duplicating_ = false;
    snapLine_.reset();
    marquee_.reset();
    bubble_.reset();
    scrubReadout_.reset();
    lastScrubSnap_.reset();
    update();
    updateHover(e->position());
}

void TimelineCanvas::scrub(QPointF p) {
    EditorModel& m = c_.model();
    double t = std::max(0.0, time(p.x()));
    const auto s = snap({t}, m.project().snapTargets({}), 6);
    if (s) t += s->first;
    lastScrubSnap_ = s ? std::optional<double>(s->second) : std::nullopt;
    m.scrub(t);
    m.scrubSound(m.playhead());
    scrubReadout_ = m.playhead();
    update();
}

void TimelineCanvas::updateMove(const Clip& grabbed, double startTime, const std::vector<Clip>& originals, QPointF p) {
    // Alt, held at any point in the drag (the mouse events say which keys are down).
    duplicating_ = (modifiers_ | QApplication::keyboardModifiers()) & Qt::AltModifier;
    setCursor(duplicating_ ? Qt::DragCopyCursor : Qt::ClosedHandCursor);
    const Project& project = c_.model().project();
    IdSet ids;
    for (const auto& c : originals) ids.insert(c.id);
    double minStart = INFINITY;
    for (const auto& c : originals) minStart = std::min(minStart, c.start);
    if (originals.empty()) minStart = 0;
    double dt = std::max(time(p.x()) - startTime, -minStart);
    snapLine_.reset();
    bool snapped = false;
    std::vector<double> candidates;
    for (const auto& c : originals) { candidates.push_back(c.start + dt); candidates.push_back(c.end() + dt); }
    // Moving, the clips' own edges aren't something to snap to. Duplicating (Alt), the originals stay where they
    // are, so their edges are: a copy goes straight against the end of the clip it's copied from.
    auto targets = project.snapTargets(duplicating_ ? IdSet{} : ids);
    targets.push_back(c_.model().playhead());
    if (auto s = snap(candidates, targets); s && minStart + dt + s->first >= -1e-9) {
        dt += s->first;
        snapLine_ = s->second;
        snapped = true;
    }
    if (!snapped) dt = std::max(project.snapFrame(grabbed.start + dt) - grabbed.start, -minStart);

    const TrackKind kind = trackKindOf(grabbed.kind);
    const auto& tracks = project.tracks(kind);
    const int previousDelta = moveTrackDelta_;
    if (auto gi = project.trackIndex(grabbed.trackID, kind)) {
        const Lane l = lane(p.y());
        if (l.kind == Lane::Kind::track && l.track.kind == kind) moveTrackDelta_ = l.index - int(*gi);
        else if (l.kind == Lane::Kind::newVideo && kind == TrackKind::video) moveTrackDelta_ = int(tracks.size()) - int(*gi);
        else if (l.kind == Lane::Kind::newAudio && kind == TrackKind::audio) moveTrackDelta_ = int(tracks.size()) - int(*gi);
    }
    std::vector<int> indices;
    for (const auto& c : originals)
        if (trackKindOf(c.kind) == kind) if (auto i = project.trackIndex(c.trackID, kind)) indices.push_back(int(*i));
    if (!indices.empty()) {
        const int lo = *std::min_element(indices.begin(), indices.end()), hi = *std::max_element(indices.begin(), indices.end());
        moveTrackDelta_ = std::max(-lo, std::min(moveTrackDelta_, int(tracks.size()) - hi));
    }
    // Clips can't be moved onto a locked track.
    for (int i : indices) {
        const int ni = i + moveTrackDelta_;
        if (ni >= 0 && ni < int(tracks.size()) && tracks[size_t(ni)].locked) { moveTrackDelta_ = previousDelta; break; }
    }

    // Linked partners on the other kind of track move the same number of tracks, so a clip taken from V1 to V2
    // has its sound go from A1 to A2 (and back), when they're on matching tracks to start with, but only into
    // space that's free: with other sound already there (music on A2), the sound stays on its own track rather
    // than writing over it.
    std::set<Uuid> movedLinks;
    for (const auto& c : originals) if (trackKindOf(c.kind) == kind && c.linkID) movedLinks.insert(*c.linkID);
    const TrackKind otherKind = kind == TrackKind::video ? TrackKind::audio : TrackKind::video;
    const auto& otherTracks = project.tracks(otherKind);
    // What the dragged clips land on: a picture taken onto V1 over a clip there may take its sound onto A1 over that
    // same clip's sound (one drag, as Premiere does); sound that isn't theirs (music) still holds it back.
    std::set<Uuid> overwritten;
    for (const auto& c : originals) {
        if (trackKindOf(c.kind) != kind) continue;
        const auto i = project.trackIndex(c.trackID, kind);
        if (!i) continue;
        const int ni = int(*i) + moveTrackDelta_;
        if (ni < 0 || ni >= int(tracks.size())) continue;
        const double start = std::max(0.0, c.start + dt), end = start + c.duration;
        for (const auto& o : project.clips)
            if (o.trackID == tracks[size_t(ni)].id && !ids.count(o.id) && o.start < end - 1e-6 && o.end() > start + 1e-6 && o.linkID) overwritten.insert(*o.linkID);
    }
    auto taken = [&](const Uuid& track, double start, double end) {
        for (const auto& o : project.clips)
            if (o.trackID == track && !ids.count(o.id) && o.start < end - 1e-6 && o.end() > start + 1e-6 && !(o.linkID && overwritten.count(*o.linkID))) return true;
        return false;
    };
    // Where each dragged clip's track is (V1 = 0), by link: its partner follows only from the matching track (A1
    // with V1), keeping them matched. On another track (a picture on V2 with its sound on A1), it stays put.
    std::map<Uuid, int> draggedTrack;
    for (const auto& c : originals)
        if (trackKindOf(c.kind) == kind && c.linkID) if (auto i = project.trackIndex(c.trackID, kind)) draggedTrack[*c.linkID] = int(*i);
    std::map<Uuid, Clip> out;
    for (Clip c : originals) {
        c.start = std::max(0.0, c.start + dt);
        if (trackKindOf(c.kind) == kind) {
            if (auto i = project.trackIndex(c.trackID, kind)) {
                const int ni = int(*i) + moveTrackDelta_;
                c.trackID = ni >= int(tracks.size()) ? (kind == TrackKind::video ? TrackToken::newVideo() : TrackToken::newAudio())
                                                     : tracks[size_t(std::max(0, ni))].id;
            }
        } else if (trackKindOf(c.kind) == otherKind && c.linkID && movedLinks.count(*c.linkID)) {
            if (auto i = project.trackIndex(c.trackID, otherKind); i && draggedTrack.count(*c.linkID) && draggedTrack[*c.linkID] == int(*i)) {
                const int ni = std::max(0, int(*i) + moveTrackDelta_);
                if (ni >= int(otherTracks.size())) {
                    c.trackID = otherKind == TrackKind::video ? TrackToken::newVideo() : TrackToken::newAudio();
                } else if (ni != int(*i) && !otherTracks[size_t(ni)].locked && !taken(otherTracks[size_t(ni)].id, c.start, c.end())) {
                    c.trackID = otherTracks[size_t(ni)].id;
                }
            }
        }
        out[c.id] = c;
    }
    overrides_ = out;
    update();
}

void TimelineCanvas::updateTrim(const Clip& grabbed, bool left, double startTime, const std::vector<Clip>& originals, QPointF p) {
    // Sticky keeps touching clips together. A left edge with nothing against it has nothing to stay attached
    // to, so it trims freely.
    const Project& project = c_.model().project();
    if (c_.model().sticky() && (!left || attachedOnLeft(originals, project)))
        return updateStickyTrim(grabbed, left, startTime, originals, p);
    IdSet ids;
    for (const auto& c : originals) ids.insert(c.id);
    const double edge = left ? grabbed.start : grabbed.end();
    double d = time(p.x()) - startTime;
    snapLine_.reset();
    std::optional<double> snapTarget;
    auto targets = project.snapTargets(ids);
    targets.push_back(c_.model().playhead());
    if (auto s = snap({edge + d}, targets)) {
        d += s->first;
        snapTarget = s->second;
    } else {
        d = project.snapFrame(edge + d) - edge;
    }
    double lo = -INFINITY, hi = INFINITY;
    for (const auto& c : originals) {
        const auto range = project.trimLimits(c, left, ids);
        lo = std::max(lo, range.first);
        hi = std::min(hi, range.second);
    }
    const double clamped = std::min(std::max(d, lo), std::max(lo, hi));
    if (clamped == d) snapLine_ = snapTarget;
    d = clamped;
    std::map<Uuid, Clip> out;
    for (Clip c : originals) {
        if (left) c.trimHead(c.start + d); else c.trimTail(c.end() + d);
        out[c.id] = c;
    }
    overrides_ = out;
    if (auto g = out.find(grabbed.id); g != out.end())
        bubble_ = std::make_pair(Timecode::string(g->second.duration, project.fps), QPointF(p.x(), p.y() - 22));
    showTrimFrame(grabbed, left);
    update();
}

/// Trimming with sticky edges: the clip keeps its start (trimming the head takes content off the front instead
/// of moving the clip), and everything touching its end follows the new end.
bool TimelineCanvas::attachedOnLeft(const std::vector<Clip>& clips, const Project& project) {
    IdSet ids;
    for (const auto& c : clips) ids.insert(c.id);
    const double tolerance = 0.5 / project.fps;
    for (const auto& c : clips) {
        if (c.start <= tolerance) return true;
        for (const auto& o : project.clips)
            if (!ids.count(o.id) && o.trackID == c.trackID && std::abs(o.end() - c.start) <= tolerance) return true;
    }
    return false;
}

/// While an edge is dragged, the viewer shows the frame it lands on: a clip's new first frame trimming its
/// start, its new last frame trimming its end (a video's own, also when its sound is the edge being dragged).
void TimelineCanvas::showTrimFrame(const Clip& grabbed, bool left) {
    const Project& p = c_.model().project();
    std::optional<Clip> video;
    if (auto it = overrides_.find(grabbed.id); it != overrides_.end() && it->second.kind == ClipKind::video) video = it->second;
    else if (grabbed.linkID)
        for (const auto& [id, c] : overrides_) if (c.linkID == grabbed.linkID && c.kind == ClipKind::video) { video = c; break; }
    if (!video || !video->assetID) return;
    const MediaAsset* asset = p.asset(video->assetID);
    if (!asset || asset->kind != MediaKind::video || !(asset->duration > 0)) return;
    const Clip& c = *video;
    const double t = left ? c.start : std::max(c.start, c.end() - 1 / std::max(1.0, p.fps));
    const double src = c.isFreeze() ? c.sourceStart : c.sourceTime(t);
    const double shown = std::min(std::max(0.0, c.playsBackwards() ? asset->duration - src : src), std::max(0.0, asset->duration - 0.02));
    // Frames are decoded one after another and shown as they come (LX has no made-ahead scrub frames yet).
    c_.sourcePreview().show(*asset, shown, left ? QStringLiteral("In") : QStringLiteral("Out"));
}

void TimelineCanvas::updateStickyTrim(const Clip& grabbed, bool left, double startTime, const std::vector<Clip>& originals, QPointF p) {
    const Project& project = c_.model().project();
    IdSet ids;
    for (const auto& c : originals) ids.insert(c.id);
    const double edge = left ? grabbed.start : grabbed.end();
    double d = time(p.x()) - startTime;
    snapLine_.reset();
    std::optional<double> snapTarget;
    auto targets = project.snapTargets(ids);
    targets.push_back(c_.model().playhead());
    if (auto s = snap({edge + d}, targets)) {
        d += s->first;
        snapTarget = s->second;
    } else {
        d = project.snapFrame(edge + d) - edge;
    }
    // Only the clip's own length and source limit what's possible; neighbours move out of the way.
    const double minDur = std::max(Project::minClip, 1 / project.fps);
    double lo = -INFINITY, hi = INFINITY;
    for (const auto& c : originals) {
        if (left) {
            if (c.playsSource()) lo = std::max(lo, -c.timelineSpan(0, c.sourceStart));
            hi = std::min(hi, c.duration - minDur);
        } else {
            lo = std::max(lo, minDur - c.duration);
            if (auto limit = project.sourceLimit(c)) hi = std::min(hi, c.timelineSpan(c.sourceStart, *limit) - c.duration);
        }
    }
    const double clamped = std::min(std::max(d, lo), std::max(lo, hi));
    if (clamped == d) snapLine_ = snapTarget;
    auto result = [&](double dd) {
        std::map<Uuid, Clip> edited;
        for (Clip c : originals) {
            if (left) {
                const double start = c.start;
                c.trimHead(c.start + dd);
                c.start = start;
            } else {
                c.trimTail(c.end() + dd);
            }
            edited[c.id] = c;
        }
        for (const auto& [id, c] : project.carried(edited)) edited.emplace(id, c);
        return edited;
    };
    auto out = result(clamped);
    if (!project.fits(out)) {
        // Something carried along would hit a clip that isn't moving: go as far as fits.
        snapLine_.reset();
        double ok = 0, bad = clamped;
        for (int i = 0; i < 24; ++i) {
            const double mid = (ok + bad) / 2;
            if (project.fits(result(mid))) ok = mid; else bad = mid;
        }
        out = result(ok);
    }
    overrides_ = out;
    if (auto g = out.find(grabbed.id); g != out.end())
        bubble_ = std::make_pair(Timecode::string(g->second.duration, project.fps), QPointF(p.x(), p.y() - 22));
    showTrimFrame(grabbed, left);
    update();
}

// MARK: Autoscroll while dragging

void TimelineCanvas::updateAutoscroll(QPointF p) {
    const double margin = 26;
    double v = 0;
    if (p.x() > width() - margin) v = std::min(34.0, (p.x() - (width() - margin)) * 0.6 + 4);
    else if (p.x() < margin && view().scrollX > 0) v = -std::min(34.0, (margin - p.x()) * 0.6 + 4);
    autoscrollVelocity_ = v;
    if (v == 0) stopAutoscroll();
    else if (!autoscroll_.isActive()) autoscroll_.start();
}

void TimelineCanvas::stopAutoscroll() { autoscroll_.stop(); }

// MARK: Scrolling & zoom

void TimelineCanvas::wheelEvent(QWheelEvent* e) {
    EditorModel& m = c_.model();
    const bool precise = !e->pixelDelta().isNull();
    double dx = precise ? e->pixelDelta().x() : e->angleDelta().x() / 120.0;
    double dy = precise ? e->pixelDelta().y() : e->angleDelta().y() / 120.0;
    if (e->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) {
        // (Qt turns a wheel with Alt held into a sideways one.)
        const double d = dy != 0 ? dy : dx;
        m.zoom(std::exp(d * (precise ? 1 : 8) * 0.012));
        return;
    }
    if (!precise) {
        dx *= 12;
        dy *= 12;
        if (std::abs(dx) < 0.01) { dx = dy; dy = 0; }
    }
    // Lock to the dominant axis so a sideways swipe never nudges the rows (and vice versa).
    if (std::abs(dx) >= std::abs(dy)) dy = 0; else dx = 0;
    m.setView(std::nullopt, view().scrollX - dx, view().scrollY - dy);
}

bool TimelineCanvas::event(QEvent* e) {
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) { c_.model().zoom(1 + g->value()); return true; }
    }
    return QWidget::event(e);
}

// MARK: Context menu

void TimelineCanvas::contextMenuEvent(QContextMenuEvent* e) {
    EditorModel& m = c_.model();
    const QPointF p = e->pos();
    QMenu menu(this);
    auto add = [&](QMenu* in, const QString& title, std::function<void()> f, bool enabled = true, std::optional<bool> checked = std::nullopt) {
        QAction* a = in->addAction(title);
        a->setEnabled(enabled);
        if (checked) { a->setCheckable(true); a->setChecked(*checked); }
        QObject::connect(a, &QAction::triggered, this, [f] { f(); });
        return a;
    };

    if (inCaptionRow(p)) {
        if (auto b = captionHit(p)) {
            if (!m.captionSelection().count(b->id())) m.selectCaptions({b->id()});
            const CaptionBlock block = *b;
            add(&menu, QStringLiteral("Edit Caption"), [&m, block] { m.seek((block.start + block.end) / 2); m.requestCaptionEdit(block.id()); });
            add(&menu, m.captionSelection().size() > 1 ? QStringLiteral("Delete Captions") : QStringLiteral("Delete Caption"), [&m] { m.deleteSelectedCaptions(); });
            add(&menu, QStringLiteral("Remove Captions from Clip"), [&m, block] { m.removeCaptions(IdSet{block.clipID}); });
            menu.addSeparator();
        }
        const bool hidden = m.project().captions && m.project().captions->hidden;
        add(&menu, hidden ? QStringLiteral("Show Captions") : QStringLiteral("Hide Captions"), [&m, hidden] { m.setCaptionsHidden(!hidden); });
        add(&menu, QStringLiteral("Delete Caption Track"), [&m] { m.removeCaptionTrack(); });
        menu.exec(e->globalPos());
        return;
    }

    if (auto h = hit(p)) {
        if (!m.selection().count(h->clip.id)) m.setSelection(m.project().linked({h->clip.id}));
        const Clip clip = h->clip;
        const double t = m.playhead();
        add(&menu, QStringLiteral("Split at Playhead"), [&m] { m.splitAtPlayhead(); }, t > clip.start && t < clip.end());
        add(&menu, QStringLiteral("Detach Audio"), [&m] { m.detachAudio(); }, m.selectionIsLinked());
        add(&menu, QStringLiteral("Reveal in Media"), [&m, id = clip.id] { m.revealInMedia(id); }, clip.assetID && m.project().asset(clip.assetID));
        if (usesSource(clip.kind)) {
            add(&menu, QStringLiteral("Freeze Frame at Playhead"), [&m] { m.freezeFrame(); },
                clip.kind == ClipKind::video && !clip.isFreeze() && t >= clip.start && t < clip.end());
            add(&menu, !clip.isReversed() ? QStringLiteral("Reverse") : QStringLiteral("Play Forwards"), [&m] { m.reverseSelection(); }, !clip.isFreeze());
        }
        if (clip.playsSource()) {
            QMenu* speed = menu.addMenu(QStringLiteral("Speed"));
            for (int pct : {25, 50, 100, 200, 400, 800, 1600, 3000})
                add(speed, QStringLiteral("%1%").arg(pct), [&m, pct] { m.setSpeed(pct / 100.0); }, true, std::abs(clip.speed * 100 - pct) < 0.5);
        }
        // Transitions on the cuts either side of this clip.
        if (clip.kind != ClipKind::audio) {
            const Project& project = m.project();
            auto transitionMenu = [&](const QString& title, const Clip& into) {
                QMenu* sub = menu.addMenu(title);
                const Uuid id = into.id;
                const std::optional<Transition> was = into.transitionIn;
                add(sub, QStringLiteral("None"), [&m, id] { m.setTransition(id, std::nullopt); }, true, !was.has_value());
                sub->addSeparator();
                for (auto kind : {TransitionKind::dissolve, TransitionKind::dipToBlack, TransitionKind::wipe}) {
                    add(sub, transitionKindLabel(kind), [&m, id, was, kind] {
                        Transition n = was.value_or(Transition());
                        n.kind = kind;
                        m.setTransition(id, n);
                    }, true, was && was->kind == kind);
                }
            };
            if (project.previousTouching(clip)) transitionMenu(QStringLiteral("Transition In"), clip);
            if (const Clip* next = project.nextTouching(clip)) transitionMenu(QStringLiteral("Transition Out"), *next);
        }
        if (clip.kind == ClipKind::audio) add(&menu, QStringLiteral("Even Out Levels"), [&m] { m.evenOutLevels(); });
        // Captions from what's said in the clip's sound.
        if (!m.project().captionTargets(m.selection()).empty()) {
            if (m.hasCaptions(m.selection())) add(&menu, QStringLiteral("Remove Captions"), [&m] { m.removeCaptions(); });
            else add(&menu, QStringLiteral("Add Captions"), [&m] { m.addCaptions(); });
        }
        if (clip.fadeIn > 0 || clip.fadeOut > 0) {
            add(&menu, QStringLiteral("Remove Fades"), [&m] {
                const IdSet ids = m.selection();
                m.mutate([&](Project& proj) { for (auto& c : proj.clips) if (ids.count(c.id)) { c.fadeIn = 0; c.fadeOut = 0; } });
            });
        }
        if (clip.kind != ClipKind::audio) {
            menu.addSeparator();
            add(&menu, QStringLiteral("Copy Grade"), [&m] { m.copyGrade(); }, m.canCopyGrade());
            int count = 0;
            for (const auto& c : m.project().clips) if (m.selection().count(c.id) && c.kind != ClipKind::audio) ++count;
            add(&menu, count > 1 ? QStringLiteral("Paste Grade to %1 Clips").arg(count) : QStringLiteral("Paste Grade"), [&m] { m.pasteGrade(); }, m.canPasteGrade());
        }
        if (clip.kind == ClipKind::video) {
            if (const MediaAsset* asset = m.project().asset(clip.assetID)) {
                const MediaAsset a = *asset;
                menu.addSeparator();
                add(&menu, QStringLiteral("Match Sequence to Clip"), [&m, a] { m.matchSequence(a); }, !m.sequenceMatches(a));
            }
        }
        menu.addSeparator();
        add(&menu, QStringLiteral("Delete"), [&m] { m.deleteSelection(); });
        add(&menu, QStringLiteral("Ripple Delete"), [&m] { m.deleteSelection(true); });
    } else {
        // Empty space on a track: offer to close the gap here, or every gap on the track.
        const Lane l = lane(p.y());
        if (l.kind == Lane::Kind::track && !l.track.locked) {
            const double t = time(p.x());
            const Uuid track = l.track.id;
            add(&menu, QStringLiteral("Delete Gap"), [&m, track, t] { m.deleteGap(track, t); }, m.project().gap(track, t).has_value());
            add(&menu, QStringLiteral("Close All Gaps on Track"), [&m, track] { m.closeAllGaps(std::vector<Uuid>{track}); });
            menu.addSeparator();
        }
        add(&menu, QStringLiteral("Add Video Track"), [&m] { m.addTrack(TrackKind::video); });
        add(&menu, QStringLiteral("Add Audio Track"), [&m] { m.addTrack(TrackKind::audio); });
        if (l.kind == Lane::Kind::track && m.project().tracks(l.track.kind).size() > 1) {
            const Uuid track = l.track.id;
            menu.addSeparator();
            add(&menu, QStringLiteral("Delete Track"), [&m, track] { m.removeTrack(track); });
        }
    }
    menu.exec(e->globalPos());
}

// MARK: Drag & drop

/// What's being dragged in from the Media panel: one item, or several picked ones, which land end to end.
std::vector<DragPayload> TimelineCanvas::payloads(const QMimeData* mime) const {
    QString s;
    if (mime->hasFormat(QLatin1String(kAssetMime))) s = QString::fromUtf8(mime->data(QLatin1String(kAssetMime)));
    else if (mime->hasText()) s = mime->text();
    if (!s.startsWith(QStringLiteral("editroom:"))) {
        // A plain asset id (an older drag).
        const Uuid id(s);
        if (!id.isNull() && c_.model().project().asset(id)) return {DragPayload::ofAsset(id)};
        return {};
    }
    const QString body = s.mid(9);
    if (body == QStringLiteral("solid")) return {DragPayload::solid()};
    if (body == QStringLiteral("text")) return {DragPayload::text()};
    std::vector<DragPayload> out;
    for (const QString& part : body.split(u',', Qt::SkipEmptyParts)) {
        const Uuid id(part);
        if (!id.isNull()) out.push_back(DragPayload::ofAsset(id));
    }
    return out;
}

bool TimelineCanvas::updateDrop(const QMimeData* mime, QPointF p) {
    EditorModel& m = c_.model();
    const bool hasFiles = mime->hasUrls();
    const auto items = hasFiles ? std::vector<DragPayload>() : payloads(mime);
    if (!hasFiles && items.empty()) return false;
    const Project& project = m.project();
    const auto& vTracks = project.videoTracks;
    const auto& aTracks = project.audioTracks;
    if (vTracks.empty() || aTracks.empty()) return false;
    Uuid vTrack = vTracks[0].id, aTrack = aTracks[0].id;
    bool overAudio = false;
    const Lane l = lane(p.y());
    switch (l.kind) {
    case Lane::Kind::track:
        if (l.track.kind == TrackKind::video) {
            vTrack = l.track.id;
            aTrack = aTracks[size_t(std::min(l.index, int(aTracks.size()) - 1))].id;
        } else {
            aTrack = l.track.id;
            vTrack = vTracks[size_t(std::min(l.index, int(vTracks.size()) - 1))].id;
            overAudio = true;
        }
        break;
    case Lane::Kind::newVideo:
        vTrack = TrackToken::newVideo();
        aTrack = aTracks[size_t(std::min(int(vTracks.size()), int(aTracks.size()) - 1))].id;
        break;
    case Lane::Kind::newAudio:
        aTrack = TrackToken::newAudio();
        vTrack = vTracks[size_t(std::min(int(aTracks.size()), int(vTracks.size()) - 1))].id;
        overAudio = true;
        break;
    case Lane::Kind::none:
        break;
    }
    // Nothing lands on a locked track: use the nearest open one, or a new track.
    auto open = [&](const Uuid& id, TrackKind kind) {
        if (!project.isLocked(id)) return id;
        for (const auto& t : project.tracks(kind)) if (!t.locked) return t.id;
        return kind == TrackKind::video ? TrackToken::newVideo() : TrackToken::newAudio();
    };
    vTrack = open(vTrack, TrackKind::video);
    aTrack = open(aTrack, TrackKind::audio);
    double duration = items.empty() ? 5 : 0;
    for (const auto& it : items) duration += m.payloadDuration(it);
    // Over the track headers (left of the first frame): the very start of the timeline, on the row it's over.
    double start = p.x() < 0 ? 0.0 : std::max(0.0, time(p.x()));
    auto targets = project.snapTargets({});
    targets.push_back(m.playhead());
    if (auto s = snap({start, start + duration}, targets); s && start + s->first >= -1e-9) {
        start = std::max(0.0, start + s->first);
        snapLine_ = s->second;
    } else {
        snapLine_.reset();
        start = project.snapFrame(start);
    }
    ghosts_.clear();
    if (!items.empty()) {
        double t = start;
        for (const auto& it : items) {
            for (const auto& c : m.makeClips(it, t, vTrack, aTrack)) ghosts_.push_back(c);
            t = project.snapFrame(t + m.payloadDuration(it));
        }
    } else {
        ghosts_.push_back(Clip(overAudio ? aTrack : vTrack, overAudio ? ClipKind::audio : ClipKind::video, start, duration));
        QStringList files;
        for (const QUrl& u : mime->urls()) if (u.isLocalFile()) files << u.toLocalFile();
        fileGhostLabel_.reset();
        if (!files.isEmpty()) {
            const QString name = QFileInfo(files.first()).completeBaseName();
            fileGhostLabel_ = files.size() > 1 ? QStringLiteral("%1 + %2 more").arg(name).arg(files.size() - 1) : name;
        }
    }
    dropInfo_ = DropInfo{vTrack, aTrack, start};
    update();
    return true;
}

void TimelineCanvas::clearDrop() {
    ghosts_.clear();
    fileGhostLabel_.reset();
    dropInfo_.reset();
    snapLine_.reset();
    update();
}

void TimelineCanvas::dragEnterEvent(QDragEnterEvent* e) {
    if (updateDrop(e->mimeData(), e->position())) e->acceptProposedAction();
}

void TimelineCanvas::dragMoveEvent(QDragMoveEvent* e) {
    if (updateDrop(e->mimeData(), e->position())) e->acceptProposedAction(); else e->ignore();
}

void TimelineCanvas::dragLeaveEvent(QDragLeaveEvent*) { clearDrop(); }

void TimelineCanvas::dropEvent(QDropEvent* e) {
    updateDrop(e->mimeData(), e->position());
    if (!dropInfo_) return;
    const DropInfo info = *dropInfo_;
    const auto pending = ghosts_;
    clearDrop();
    EditorModel& m = c_.model();
    if (e->mimeData()->hasUrls()) {
        QStringList paths;
        for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) paths << u.toLocalFile();
        c_.importFiles(paths, true, [&m, info](std::vector<MediaAsset> assets) {
            double t = info.start;
            std::vector<Clip> clips;
            for (const auto& a : assets) {
                const DragPayload payload = DragPayload::ofAsset(a.id);
                for (const auto& c : m.makeClips(payload, t, info.video, info.audio)) clips.push_back(c);
                t += m.payloadDuration(payload);
            }
            if (!clips.empty()) {
                m.insert(clips);
                // Only the new timeline clips are picked, so Delete takes them off the timeline, not out of Media.
                m.clearMediaPicks();
            }
        });
        e->acceptProposedAction();
        return;
    }
    if (pending.empty()) return;
    m.insert(pending);
    // Only the new timeline clips are picked, so Delete takes them off the timeline, not out of Media.
    m.clearMediaPicks();
    setFocus();
    e->acceptProposedAction();
}

// MARK: Drawing

void TimelineCanvas::paintEvent(QPaintEvent*) {
    QPainter ctx(this);
    ctx.setRenderHint(QPainter::Antialiasing);
    EditorModel& m = c_.model();
    const Project& project = m.project();
    layout_ = TimelineLayout(project, view().rowHeight);
    const double ruler = TimelineLayout::ruler;
    const double W = width(), H = height();
    ctx.fillRect(QWidget::rect(), Theme::bg());

    // Lanes
    for (const auto& row : layout_.rows) {
        const QRectF r(0, ruler + row.y - view().scrollY, W, row.height);
        if (r.bottom() <= ruler || r.top() >= H) continue;
        const bool off = row.track.locked || (row.track.kind == TrackKind::video ? row.track.hidden : !project.isAudible(row.track));
        ctx.fillRect(r, off ? mix(Theme::bg(), Theme::lane(), 0.4) : Theme::lane());
    }
    // The caption track: its captions as blocks of their words.
    if (layout_.captionY) {
        const QRectF lane(0, ruler + *layout_.captionY - view().scrollY, W, TimelineLayout::captionRow);
        if (lane.bottom() > ruler && lane.top() < H) {
            const bool hidden = project.captions && project.captions->hidden;
            ctx.fillRect(lane, hidden ? mix(Theme::bg(), Theme::lane(), 0.4) : Theme::lane());
            drawCaptions(ctx, lane, hidden);
        }
    }
    // An empty timeline says how to fill it, across V1.
    if (project.clips.empty() && ghosts_.empty() && !project.videoTracks.empty()) {
        if (auto band = layout_.band(project.videoTracks.front().id)) {
            ctx.setFont(hintFont());
            ctx.setPen(Theme::hint());
            ctx.drawText(QRectF(0, ruler + band->first - view().scrollY, W, band->second), Qt::AlignCenter,
                         QStringLiteral("Drag media here, or double-click it in the library"));
        }
    }
    // "New track" drop zones, and the lanes a drag will land on.
    std::set<Uuid> targets;
    for (const auto& g : ghosts_) targets.insert(g.trackID);
    for (const auto& [id, c] : overrides_) targets.insert(c.trackID);
    for (const auto& [token, zoneY] : {std::make_pair(TrackToken::newVideo(), layout_.videoZoneY), std::make_pair(TrackToken::newAudio(), layout_.audioZoneY)}) {
        if (!targets.count(token)) continue;
        QColor a = Theme::accent();
        a.setAlphaF(0.1);
        ctx.fillRect(QRectF(0, ruler + zoneY - view().scrollY, W, TimelineLayout::zone), a);
    }
    for (const auto& row : layout_.rows)
        if (targets.count(row.track.id)) ctx.fillRect(QRectF(0, ruler + row.y - view().scrollY, W, row.height), Theme::ink(0.035));

    // Clips. While Alt-dragging, the originals stay put and the dragged ones are the copies.
    IdSet lifted;
    if (!duplicating_) for (const auto& [id, c] : overrides_) lifted.insert(id);
    for (const auto& c : project.clips) if (!lifted.count(c.id)) drawClip(ctx, c, project.isLocked(c.trackID) ? 0.45 : 1);
    for (const auto& [id, c] : overrides_) drawClip(ctx, c, 0.82);
    for (const auto& c : ghosts_) drawClip(ctx, c, 0.55);

    // Transitions: a hatched block over the stretch each one covers.
    for (const auto& b : project.clips) {
        if (!b.transitionIn || lifted.count(b.id)) continue;
        const Clip* a = project.previousTouching(b);
        if (!a || lifted.count(a->id)) continue;
        auto span = project.transitionSpan(*a, b);
        auto band = rect(b);
        if (!span || !band) continue;
        const QRectF r(x(span->start), band->top() + 3, std::max(4.0, x(span->end) - x(span->start)), band->height() - 6);
        if (r.right() <= 0 || r.left() >= W) continue;
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        ctx.save();
        ctx.setClipPath(path);
        ctx.fillRect(r, black(0.35));
        ctx.setPen(QPen(white(0.35), 2));
        for (double sx = r.left() - r.height(); sx < r.right(); sx += 7) ctx.drawLine(QPointF(sx, r.bottom()), QPointF(sx + r.height(), r.top()));
        ctx.restore();
        ctx.setPen(QPen(white(m.selection().count(b.id) ? 1 : 0.75), 1.5));
        ctx.setBrush(Qt::NoBrush);
        ctx.drawPath(path);
    }

    // Skim line: where the viewer is showing, while the playhead stays put.
    if (auto t = m.skimTime()) {
        const double sx = x(*t);
        ctx.fillRect(QRectF(sx - 0.5, ruler, 1, H - ruler), Theme::ink(0.55));
        ctx.fillRect(QRectF(sx - 3, ruler - 6, 6, 6), Theme::ink(0.55));
    }
    // Cut preview
    if (cutHover_) {
        const IdSet group = project.linked({cutHover_->first});
        for (const auto& c : project.clips) {
            if (!group.count(c.id) || !(cutHover_->second > c.start && cutHover_->second < c.end())) continue;
            if (auto r = rect(c)) ctx.fillRect(QRectF(x(cutHover_->second) - 0.75, r->top(), 1.5, r->height()), Qt::white);
        }
    }
    // Snap guide: a line with a dot at each end of the clips it joins.
    if (snapLine_) {
        const double sx = x(*snapLine_);
        double top = INFINITY, bottom = -INFINITY;
        auto spread = [&](const Clip& c) { if (auto r = rect(c)) { top = std::min(top, r->top()); bottom = std::max(bottom, r->bottom()); } };
        for (const auto& [id, c] : overrides_) spread(c);
        for (const auto& c : ghosts_) spread(c);
        top = std::max(ruler, (std::isfinite(top) ? top : ruler) - 4);
        bottom = std::min(H, (std::isfinite(bottom) ? bottom : H) + 4);
        ctx.fillRect(QRectF(sx - 0.5, top, 1, bottom - top), Theme::ink(0.9));
        ctx.setPen(Qt::NoPen);
        ctx.setBrush(Theme::ink(0.9));
        for (double yy : {top, bottom}) ctx.drawEllipse(QPointF(sx, yy), 3, 3);
    }
    // Marquee
    if (marquee_) {
        QColor fill = Theme::accent(), edge = Theme::accent();
        fill.setAlphaF(0.12);
        edge.setAlphaF(0.8);
        ctx.fillRect(*marquee_, fill);
        ctx.setPen(QPen(edge, 1));
        ctx.setBrush(Qt::NoBrush);
        ctx.drawRect(marquee_->adjusted(0.5, 0.5, -0.5, -0.5));
    }
    // Outside the in/out range is dimmed; markers run a faint line down through the tracks.
    if (auto r = project.playRange()) {
        const double a = x(r->start), b = x(r->end);
        if (a > 0) ctx.fillRect(QRectF(0, ruler, a, H - ruler), black(0.32));
        if (b < W) ctx.fillRect(QRectF(b, ruler, W - b, H - ruler), black(0.32));
    }
    for (const auto& mk : project.markerList()) {
        const double mx = std::round(x(mk.time));
        if (mx <= -1 || mx >= W + 1) continue;
        ctx.fillRect(QRectF(mx, ruler, 1, H - ruler), Theme::hex(Marker::hex(mk.tint), 0.45));
    }

    drawRuler(ctx, project);

    // Value bubble
    if (bubble_) {
        const QFontMetricsF fm(bubbleFont());
        const double tw = fm.horizontalAdvance(bubble_->first), th = fm.height();
        QRectF r(bubble_->second.x() - tw / 2 - 7, bubble_->second.y() - th / 2 - 3, tw + 14, th + 6);
        r.moveLeft(std::min(std::max(4.0, r.left()), W - r.width() - 4));
        r.moveTop(std::max(ruler + 2, r.top()));
        ctx.setPen(Qt::NoPen);
        ctx.setBrush(QColor::fromRgbF(0.05f, 0.05f, 0.05f, 0.92f));
        ctx.drawRoundedRect(r, 5, 5);
        ctx.setFont(bubbleFont());
        ctx.setPen(Qt::white);
        ctx.drawText(r, Qt::AlignCenter, bubble_->first);
    }
    // The playhead's time beside its flag while it's being dragged.
    const std::optional<double> readout = scrubReadout_ ? scrubReadout_ : (m.showPlayheadTime() ? std::optional<double>(m.playhead()) : std::nullopt);
    if (readout) {
        const QString text = Timecode::string(*readout, project.fps);
        const QFontMetricsF fm(readoutFont());
        QRectF r(x(*readout) + 10, 4, fm.horizontalAdvance(text) + 12, fm.height() + 4);
        if (r.right() > W - 4) r.moveLeft(x(*readout) - 10 - r.width());
        ctx.setPen(Qt::NoPen);
        ctx.setBrush(Theme::accent());
        ctx.drawRoundedRect(r, 4, 4);
        ctx.setFont(readoutFont());
        ctx.setPen(Qt::white);
        ctx.drawText(r, Qt::AlignCenter, text);
    }
}

/// The captions as they'll be: while clips are dragged or trimmed, laid out from where they are now.
std::vector<CaptionBlock> TimelineCanvas::shownCaptions() const {
    if (overrides_.empty()) return c_.model().captionBlocks();
    Project q = c_.model().project();
    for (const auto& [id, c] : overrides_) {
        if (duplicating_) {
            // Alt-drag: the copies, as well as the originals where they were.
            Clip copy = c;
            copy.id = newUuid();
            q.clips.push_back(copy);
        } else if (auto i = q.index(id)) {
            q.clips[*i] = c;
        }
    }
    const EditorHost* host = c_.model().host();
    return CaptionLayout::blocks(q, [host](const MediaAsset& a) { return host->transcript(a.id); });
}

void TimelineCanvas::drawCaptions(QPainter& ctx, const QRectF& lane, bool dim) {
    const auto blocks = shownCaptions();
    if (blocks.empty()) {
        ctx.setFont(hintFont());
        ctx.setPen(Theme::hint());
        ctx.drawText(QRectF(12, lane.top(), lane.width() - 24, lane.height()), Qt::AlignVCenter | Qt::AlignLeft,
                     c_.model().captionsWaiting() ? QStringLiteral("Listening for what's said…")
                     : !c_.transcripts().unsupportedLanguage().isEmpty() ? QStringLiteral("%1 speech can't be transcribed on this computer yet").arg(c_.transcripts().unsupportedLanguage())
                     : QStringLiteral("No speech heard in the captioned clips"));
        return;
    }
    const QColor fill = Theme::clip(ClipKind::text);
    for (const auto& b : blocks) {
        auto full = captionRect(b);
        if (!full || full->right() <= -4 || full->left() >= width() + 4) continue;
        const QRectF r = full->adjusted(std::min(0.5, full->width() / 4), 3, -std::min(0.5, full->width() / 4), -3);
        const double rad = std::min(5.0, r.width() / 2);
        QPainterPath path;
        path.addRoundedRect(r, rad, rad);
        ctx.save();
        ctx.setOpacity(dim ? 0.45 : 1);
        ctx.fillPath(path, fill);
        if (r.width() > 18) {
            ctx.setClipPath(path);
            ctx.setFont(captionFont());
            ctx.setPen(white(0.92));
            ctx.drawText(QRectF(std::max(r.left(), 0.0) + 6, r.top(), 100000, r.height()), Qt::AlignVCenter | Qt::AlignLeft, b.text());
        }
        ctx.restore();
        if (c_.model().captionSelection().count(b.id())) {
            ctx.setPen(QPen(Theme::selection(), 1.5));
            ctx.setBrush(Qt::NoBrush);
            ctx.drawPath(path);
        }
    }
}

void TimelineCanvas::drawRuler(QPainter& ctx, const Project& project) {
    const double ruler = TimelineLayout::ruler, W = width();
    ctx.fillRect(QRectF(0, 0, W, ruler), Theme::panel());
    ctx.fillRect(QRectF(0, ruler - 1, W, 1), Theme::stroke());
    const double fps = project.fps, f = 1 / fps;
    const std::vector<std::pair<double, int>> steps = {
        {f, 1}, {2 * f, 2}, {5 * f, 5}, {10 * f, 5}, {1, 4}, {2, 4}, {5, 5}, {10, 5}, {15, 3}, {30, 6},
        {60, 6}, {120, 4}, {300, 5}, {600, 5}, {900, 3}, {1800, 6}, {3600, 6}, {7200, 4}, {18000, 5}, {36000, 5}};
    std::pair<double, int> step = steps.back();
    for (const auto& s : steps) if (s.first * pps() >= 86 && (s.first >= 1 || s.first < 0.5)) { step = s; break; }
    const auto [major, subdivisions] = step;
    const double t0 = time(0), t1 = time(W);
    long k = long(std::floor(t0 / major));
    const bool showFrames = major < 1;
    ctx.setFont(rulerFont());
    const QFontMetricsF fm(rulerFont());
    while (double(k) * major <= t1) {
        const double t = double(k) * major;
        const double px = x(t);
        if (t >= 0) {
            ctx.fillRect(QRectF(std::round(px), ruler - 9, 1, 8), Theme::ink(0.28));
            QString label = Timecode::shortString(t);
            if (showFrames) {
                if (Timecode::currentStyle() == TimeStyle::simple) {
                    // Zoomed in, with plain times: hundredths, as the viewer shows them.
                    label = Timecode::simple(t);
                } else {
                    const int frames = int(std::round(t * fps)) % std::max(1, int(std::round(fps)));
                    label = Timecode::shortString(std::floor(t + 1e-6)) + QString::asprintf(":%02d", frames);
                }
            }
            ctx.setPen(Theme::textFaint());
            ctx.drawText(QPointF(px + 5, 5 + fm.ascent()), label);
        }
        if (subdivisions > 1) {
            for (int j = 1; j < subdivisions; ++j) {
                const double mt = t + major * j / subdivisions;
                if (mt >= 0) ctx.fillRect(QRectF(std::round(x(mt)), ruler - 5, 1, 4), Theme::ink(0.13));
            }
        }
        ++k;
    }
    // Markers: a coloured flag with the name beside it.
    for (const auto& mk : project.markerList()) {
        const double mx = std::round(x(mk.time));
        const double lw = markerLabelWidth(mk);
        if (mx + 12 + lw <= 0 || mx >= W + 1) continue;
        const QColor color = Theme::hex(Marker::hex(mk.tint));
        ctx.fillRect(QRectF(mx + 1, 3, lw + 14, ruler - 5), Theme::panel());
        ctx.fillRect(QRectF(mx, 4, 1, ruler - 5), color);
        QPainterPath flag;
        flag.moveTo(mx, 4);
        flag.lineTo(mx + 8, 4);
        flag.lineTo(mx + 6, 8);
        flag.lineTo(mx + 8, 12);
        flag.lineTo(mx, 12);
        flag.closeSubpath();
        ctx.fillPath(flag, color);
        ctx.setFont(markerFont());
        QColor tc = Theme::text();
        tc.setAlphaF(0.92);
        ctx.setPen(tc);
        ctx.drawText(QPointF(mx + 11, 5 + QFontMetricsF(markerFont()).ascent()), mk.name);
    }
    // In/out: the stretch between is lit, with a white bracket at each end.
    if (project.inPoint || project.outPoint) {
        if (auto r = project.playRange()) ctx.fillRect(QRectF(x(r->start), 0, x(r->end) - x(r->start), ruler - 1), Theme::ink(0.06));
        for (const auto& [edge, inward] : {std::make_pair(project.inPoint, 1.0), std::make_pair(project.outPoint, -1.0)}) {
            if (!edge) continue;
            const double ex = std::round(x(*edge));
            ctx.fillRect(QRectF(ex - 1, 2, 2, ruler - 3), Theme::ink());
            ctx.fillRect(QRectF(inward > 0 ? ex - 1 : ex - 5, 2, 6, 2), Theme::ink());
            ctx.fillRect(QRectF(inward > 0 ? ex - 1 : ex - 5, ruler - 3, 6, 2), Theme::ink());
        }
    }
}

double TimelineCanvas::markerLabelWidth(const Marker& m) const { return QFontMetricsF(markerFont()).horizontalAdvance(m.name); }

/// The marker whose flag or name is under a point on the ruler (the last drawn wins).
std::optional<Uuid> TimelineCanvas::markerHit(QPointF p) const {
    const auto list = c_.model().project().markerList();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const double mx = x(it->time);
        if (p.x() >= mx - 4 && p.x() <= mx + 12 + markerLabelWidth(*it)) return it->id;
    }
    return std::nullopt;
}

std::optional<bool> TimelineCanvas::rangeEdgeHit(QPointF p) const {
    const Project& project = c_.model().project();
    if (project.inPoint && std::abs(p.x() - x(*project.inPoint)) < 5) return true;
    if (project.outPoint && std::abs(p.x() - x(*project.outPoint)) < 5) return false;
    return std::nullopt;
}

// MARK: Marker panel

void TimelineCanvas::showMarkerPanel() {
    EditorModel& m = c_.model();
    const auto id = m.editingMarker;
    if (!id) {
        if (markerPanel_) markerPanel_->close();
        return;
    }
    double t = -1;
    for (const auto& mk : m.project().markerList()) if (mk.id == *id) t = mk.time;
    if (t < 0) return;
    if (!markerPanel_) {
        markerPanel_ = new MarkerPanel(m, this);
        connect(markerPanel_, &MarkerPanel::closed, this, [this] {
            markerPanel_ = nullptr;
            if (c_.model().editingMarker) c_.model().setEditingMarker(std::nullopt);
            // Hand focus back to the editor, so keys and menus go where they should.
            setFocus();
        });
    }
    markerPanel_->setMarker(*id);
    // Above the flag: the timeline sits at the bottom of the window, so there's rarely room below.
    const double mx = std::min(std::max(4.0, x(t)), width() - 4.0);
    markerPanel_->showAbove(mapToGlobal(QPoint(int(mx), 0)));
}

// MARK: Clips

void TimelineCanvas::drawClip(QPainter& ctx, const Clip& c, double alpha) {
    EditorModel& m = c_.model();
    auto fullR = rect(c);
    if (!fullR) return;
    const QRectF full = *fullR;
    if (full.right() <= -4 || full.left() >= width() + 4 || full.bottom() <= TimelineLayout::ruler || full.top() >= height()) return;
    const double inset = std::min(0.5, full.width() / 4);
    const QRectF r = full.adjusted(inset, 2, -inset, -2);
    const bool thumbs = m.thumbnails();
    const QColor base = thumbs ? Theme::clip(c.kind) : Theme::clipType(c.kind);
    const bool selected = m.selection().count(c.id) > 0;
    const bool hovered = hover_ && hover_->first == c.id;
    std::optional<Zone> zone = hovered ? std::optional<Zone>(hover_->second) : std::nullopt;
    // Hovering a selected clip's edge shows that edge on every selected clip too, since a drag trims them all;
    // and while trimming, every clip being trimmed keeps its edge lit.
    if (!hovered && hover_ && (hover_->second == Zone::left || hover_->second == Zone::right) && m.selection().count(hover_->first)
        && m.selection().count(c.id) && !m.project().isLocked(c.trackID))
        zone = hover_->second;
    if (drag_.kind == Drag::Kind::trim)
        for (const auto& o : drag_.originals) if (o.id == c.id) { zone = drag_.left ? Zone::left : Zone::right; break; }
    const double radius = std::min(7.0, r.width() / 2);
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    const QRectF visible = r.intersected(QRectF(QWidget::rect()).adjusted(-2, 0, 2, 0));
    const MediaAsset* asset = m.project().asset(c.assetID);
    const bool offline = usesSource(c.kind) || c.kind == ClipKind::image ? (!asset || c_.media().looksOffline(*asset)) : false;
    bool isGhost = false;
    for (const auto& g : ghosts_) if (g.id == c.id) isGhost = true;

    ctx.save();
    ctx.setOpacity(alpha);
    ctx.setClipPath(path);
    ctx.fillRect(r, offline && !isGhost ? Theme::offline() : base);
    switch (c.kind) {
    case ClipKind::video:
        if (thumbs && asset) drawFilmstrip(ctx, c, *asset, r, visible);
        break;
    case ClipKind::image:
        if (thumbs && asset) {
            const QImage still = c_.thumbnails().get(*asset, 0, 240);
            if (!still.isNull()) {
                const QImage img = graded(still, c.grade);
                const double tileW = std::max(20.0, r.height() * img.width() / std::max(1, img.height()));
                double tx = r.left() + std::floor(std::max(0.0, visible.left() - r.left()) / tileW) * tileW;
                ctx.setOpacity(alpha * 0.85);
                while (tx < visible.right()) {
                    drawImage(ctx, img, QRectF(tx, r.top(), tileW, r.height()));
                    tx += tileW;
                }
                ctx.setOpacity(alpha);
            }
        }
        break;
    case ClipKind::audio:
        drawWaveform(ctx, c, r, visible);
        break;
    case ClipKind::solid:
        ctx.fillRect(QRectF(r.left(), r.bottom() - 5, r.width(), 5), QColor::fromRgbF(float(c.color.r), float(c.color.g), float(c.color.b), float(c.color.a)));
        break;
    case ClipKind::text:
        break;
    }

    // Title strip
    if (r.width() > 26 && r.height() > 20) {
        if (thumbs && (c.kind == ClipKind::video || c.kind == ClipKind::image)) ctx.fillRect(QRectF(r.left(), r.top(), r.width(), 17), black(0.45));
        // Files being dropped have no library item yet: their outline shows the file's name, not "Missing".
        const bool fileGhost = !c.assetID && usesSource(c.kind) && std::any_of(ghosts_.begin(), ghosts_.end(), [&](const Clip& g) { return g.id == c.id; });
        QString label = fileGhost ? fileGhostLabel_.value_or(QString()) : m.project().name(c);
        if (c.isFreeze()) label += QStringLiteral("   Freeze");
        else if (std::abs(c.speed - 1) > 0.001) label += QStringLiteral("   %1%").arg(int(std::round(c.speed * 100)));
        if (c.isReversed()) label += QStringLiteral("   Reversed");
        if (c.kind == ClipKind::audio && std::abs(c.gainDB) > 0.05) label += QStringLiteral("   ") + formatDB(c.gainDB);
        double lx = std::max(r.left(), 0.0) + 8;
        if (r.width() > 36 && c.fadeIn > 0) lx = std::max(lx, fadeHandleX(c, r, true) + 10);
        ctx.save();
        ctx.setClipRect(QRectF(r.left(), r.top(), std::max(0.0, r.width() - 16), 18), Qt::IntersectClip);
        shadowText(ctx, QPointF(lx, r.top() + 2.5), label, labelFont());
        // A graded clip says so, and which Look: "● Warm film", or "● Graded" for adjustments only. A grade that's
        // off is still there, faint and struck through, so it isn't forgotten.
        if (c.grade && !c.grade->isNeutral() && c.kind != ClipKind::audio) {
            const double gx = lx + QFontMetricsF(labelFont()).horizontalAdvance(label) + 9;
            ctx.save();
            if (c.grade->off) ctx.setOpacity(0.45);
            drawGradeWheel(ctx, QPointF(gx + 4.5, r.top() + 9.5), 4.5);
            QFont f = labelFont();
            if (c.grade->off) f.setStrikeOut(true);
            shadowText(ctx, QPointF(gx + 13, r.top() + 2.5), c.grade->look ? c.grade->lookLabel() : QStringLiteral("Graded"), f);
            ctx.restore();
        }
        ctx.restore();
    }

    // Fades: the curve, with the faded part shaded.
    for (bool isIn : {true, false}) {
        const double w = (isIn ? c.fadeIn : c.fadeOut) * pps();
        if (w <= 0.5) continue;
        const FadeCurve curve = isIn ? c.inCurve() : c.outCurve();
        const double edge = isIn ? r.left() : r.right();
        const double dir = isIn ? 1 : -1;
        QPainterPath line;
        for (int i = 0; i <= 24; ++i) {
            const double n = i / 24.0;
            const QPointF pt(edge + dir * n * w, r.bottom() - applyFadeCurve(curve, n) * r.height());
            if (i == 0) line.moveTo(pt); else line.lineTo(pt);
        }
        QPainterPath shade = line;
        shade.lineTo(edge, r.top());
        shade.closeSubpath();
        ctx.fillPath(shade, black(0.55));
        ctx.setPen(QPen(white(0.85), 1));
        ctx.setBrush(Qt::NoBrush);
        ctx.drawPath(line);
    }
    // Gain line
    if (c.kind == ClipKind::audio && r.width() > 8) {
        const double gy = std::round(r.top() + gainNorm(c.gainDB) * r.height()) + 0.5;
        const bool hot = zone == Zone::gain;
        ctx.setPen(QPen(white(hot ? 1 : 0.5), hot ? 2 : 1));
        ctx.drawLine(QPointF(r.left(), gy), QPointF(r.right(), gy));
    }
    // Edge affordance
    if (zone == Zone::left || zone == Zone::right) ctx.fillRect(QRectF(zone == Zone::left ? r.left() : r.right() - 3.5, r.top(), 3.5, r.height()), Qt::white);
    ctx.restore();

    // Border
    ctx.save();
    ctx.setOpacity(alpha);
    ctx.setBrush(Qt::NoBrush);
    if (selected) ctx.setPen(QPen(Theme::selection(), 1.5));
    else if (hovered) ctx.setPen(QPen(white(0.6), 1));
    // Flat colour clips are edged in a lighter tone of their own colour, so cuts between clips of one type show.
    else ctx.setPen(QPen(thumbs ? Theme::clipStroke() : mix(base, Qt::white, 0.18), 1));
    ctx.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    // Fade handles: only for fades that are set. The longer fade's dot is drawn last, on top.
    if (r.width() > 36 && alpha == 1 && (c.fadeIn > 0 || c.fadeOut > 0)) {
        const std::vector<bool> order = c.fadeIn > c.fadeOut ? std::vector<bool>{false, true} : std::vector<bool>{true, false};
        for (bool isIn : order) {
            if (!((isIn ? c.fadeIn : c.fadeOut) > 0)) continue;
            const bool hot = zone == (isIn ? Zone::fadeIn : Zone::fadeOut);
            const double hx = fadeHandleX(c, r, isIn);
            const double rad = hot ? 5.5 : 4;
            ctx.setBrush(hot ? Theme::accent() : QColor(Qt::white));
            ctx.setPen(QPen(black(0.55), 1));
            ctx.drawEllipse(QPointF(hx, r.top() + 8), rad, rad);
        }
    }
    ctx.restore();
}

/// A small colour wheel: the mark for a graded clip.
void TimelineCanvas::drawGradeWheel(QPainter& ctx, QPointF centre, double radius) {
    const double hues[6][3] = {{1, 0.35, 0.29}, {1, 0.82, 0.29}, {0.35, 0.86, 0.42}, {0.29, 0.72, 1}, {0.6, 0.42, 1}, {1, 0.35, 0.6}};
    ctx.save();
    ctx.setPen(Qt::NoPen);
    ctx.setBrush(black(0.5));
    ctx.drawEllipse(centre, radius + 0.75, radius + 0.75);
    const QRectF box(centre.x() - radius, centre.y() - radius, radius * 2, radius * 2);
    for (int i = 0; i < 6; ++i) {
        // Clockwise from the top, as the Mac's (y down). Qt angles run anticlockwise from 3 o'clock.
        QPainterPath slice;
        slice.moveTo(centre);
        slice.arcTo(box, 90 - i * 60.0, -(60 + 1.2));
        slice.closeSubpath();
        ctx.setBrush(QColor::fromRgbF(float(hues[i][0]), float(hues[i][1]), float(hues[i][2])));
        ctx.drawPath(slice);
    }
    ctx.restore();
}

/// A thumbnail with its clip's grade applied: the same 32³ colour cube the viewer and export use, on the
/// stored values (no colour management, as the Mac's).
QImage TimelineCanvas::graded(const QImage& img, const std::optional<ColorGrade>& grade) {
    if (!grade || grade->isNeutral() || img.isNull()) return img;
    auto cube = GradeBaker::cube(grade);
    if (!cube) return img;
    const QString key = QString::number(img.cacheKey()) + u'/'
                      + QString::number(qHash(QByteArray(reinterpret_cast<const char*>(cube->data()), qsizetype(cube->size() * sizeof(float)))));
    if (auto it = gradedThumbs_.find(key); it != gradedThumbs_.end()) return *it;
    const int n = 32;
    const std::vector<float>& lut = *cube;
    QImage out = img.convertToFormat(QImage::Format_RGBX8888);
    for (int y = 0; y < out.height(); ++y) {
        uchar* row = out.scanLine(y);
        for (int xx = 0; xx < out.width(); ++xx) {
            uchar* px = row + xx * 4;
            const float c[3] = {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
            int i0[3];
            float f[3];
            for (int k = 0; k < 3; ++k) {
                const float s = std::clamp(c[k], 0.0f, 1.0f) * (n - 1);
                i0[k] = std::min(n - 2, int(s));
                f[k] = s - float(i0[k]);
            }
            auto at = [&](int r, int g, int b, int ch) { return lut[size_t(((b * n + g) * n + r) * 4 + ch)]; };
            for (int ch = 0; ch < 3; ++ch) {
                const float c00 = at(i0[0], i0[1], i0[2], ch) * (1 - f[0]) + at(i0[0] + 1, i0[1], i0[2], ch) * f[0];
                const float c10 = at(i0[0], i0[1] + 1, i0[2], ch) * (1 - f[0]) + at(i0[0] + 1, i0[1] + 1, i0[2], ch) * f[0];
                const float c01 = at(i0[0], i0[1], i0[2] + 1, ch) * (1 - f[0]) + at(i0[0] + 1, i0[1], i0[2] + 1, ch) * f[0];
                const float c11 = at(i0[0], i0[1] + 1, i0[2] + 1, ch) * (1 - f[0]) + at(i0[0] + 1, i0[1] + 1, i0[2] + 1, ch) * f[0];
                const float v = (c00 * (1 - f[1]) + c10 * f[1]) * (1 - f[2]) + (c01 * (1 - f[1]) + c11 * f[1]) * f[2];
                px[ch] = uchar(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
            }
        }
    }
    // Dragging a grade slider makes a new grade every frame; old ones are let go in bulk.
    if (gradedThumbs_.size() > 600) gradedThumbs_.clear();
    gradedThumbs_.insert(key, out);
    return out;
}

void TimelineCanvas::drawImage(QPainter& ctx, const QImage& img, const QRectF& fill) {
    const double iw = img.width(), ih = img.height();
    if (iw <= 0 || ih <= 0) return;
    const double s = std::max(fill.width() / iw, fill.height() / ih);
    const double dw = iw * s, dh = ih * s;
    ctx.save();
    ctx.setClipRect(fill, Qt::IntersectClip);
    ctx.drawImage(QRectF(fill.center().x() - dw / 2, fill.center().y() - dh / 2, dw, dh), img);
    ctx.restore();
}

void TimelineCanvas::drawFilmstrip(QPainter& ctx, const Clip& c, const MediaAsset& asset, const QRectF& r, const QRectF& visible) {
    if (visible.width() <= 0 || asset.height <= 0) return;
    Thumbnails& thumbs = c_.thumbnails();
    const double aspect = double(asset.width) / asset.height;
    const double tileW = std::max(24.0, r.height() * aspect);
    const int box = 240;
    if (c.isRamped()) {
        // Speed changes across it: evenly spaced frames from the clip's start, each showing the footage playing
        // at that moment, taken from a fixed grid of times in the file (so while a ramp is dragged the same
        // frames are reused). Until a frame is made, a neighbour on the grid stands in, so nothing blinks.
        const double average = std::max(0.01, c.sourceLength() / std::max(1e-6, c.duration));
        double grid = 1.0 / 16;
        while (grid / average * pps() < tileW && grid < 16384) grid *= 2;
        auto frame = [&](double ts) -> QImage {
            const double k = std::round(ts / grid);
            auto at = [&](double kk) {
                const double s = kk * grid;
                const double shown = c.playsBackwards() ? asset.duration - s : s;
                return std::min(std::max(0.0, shown), std::max(0.0, asset.duration - 0.05));
            };
            QImage img = thumbs.get(asset, at(k), box);
            if (!img.isNull()) return img;
            for (double d : {1.0, -1.0, 2.0, -2.0}) {
                img = thumbs.cached(asset, at(k + d), box);
                if (!img.isNull()) return img;
            }
            return {};
        };
        double xx = r.left() + std::floor(std::max(0.0, visible.left() - r.left()) / tileW) * tileW;
        int drawn = 0;
        ctx.save();
        ctx.setOpacity(ctx.opacity() * 0.88);
        while (xx < std::min(r.right(), visible.right()) && drawn < 240) {
            const double t = c.start + (xx + tileW / 2 - r.left()) / pps();
            const QImage img = frame(c.sourceTime(std::min(t, c.end())));
            if (!img.isNull()) drawImage(ctx, graded(img, c.grade), QRectF(xx, r.top(), std::max(1.0, tileW - 1), r.height()));
            xx += tileW;
            ++drawn;
        }
        ctx.restore();
        return;
    }
    // Source seconds per tile: the smallest power of two that keeps tiles at least one thumbnail wide.
    double step = 1.0 / 16;
    while (step / c.speed * pps() < tileW && step < 16384) step *= 2;
    auto srcAt = [&](double px) { return c.sourceStart + (px - r.left()) / pps() * c.speed; };
    long k = long(std::floor(srcAt(visible.left()) / step));
    const long kEnd = long(std::ceil(srcAt(visible.right()) / step));
    const double w = step / c.speed * pps();
    int drawn = 0;
    ctx.save();
    ctx.setOpacity(ctx.opacity() * 0.88);
    while (k <= kEnd && drawn < 240) {
        const double ts = double(k) * step;
        const double tx = r.left() + (ts - c.sourceStart) / c.speed * pps();
        const double shown = c.playsBackwards() ? asset.duration - ts : ts;
        const double sample = c.isFreeze() ? c.sourceStart : std::min(std::max(0.0, shown), std::max(0.0, asset.duration - 0.05));
        const QImage img = thumbs.get(asset, sample, box);
        // Frames sit side by side with a 1 px dark gap, like a contact sheet.
        if (!img.isNull()) drawImage(ctx, graded(img, c.grade), QRectF(tx, r.top(), std::max(1.0, w - 1), r.height()));
        ++k;
        ++drawn;
    }
    ctx.restore();
}

/// The sound as one smooth filled shape of average loudness, growing up from the bottom of the clip, so taller
/// is louder. It uses the clip's whole height, running up behind the name. Silence drops to a thin line, so
/// pauses are easy to find and cut on. Samples sit on a grid tied to the source, so the shape doesn't shimmer
/// while scrolling.
void TimelineCanvas::drawWaveform(QPainter& ctx, const Clip& c, const QRectF& r, const QRectF& visible) {
    if (!c.assetID) return;
    const Waveform* wf = c_.sound().waveform(*c.assetID, false);
    if (!wf || visible.width() <= 0 || wf->peaks.empty()) return;
    const double top = r.top() + 2, base = r.bottom() - 1;
    if (base - top <= 4) return;
    const double room = base - top;
    const double gain = dbToLinear(c.gainDB);
    // Average loudness sits well under the peaks, so it's lifted to fill the lane, with headroom.
    const double lift = 2.0, headroom = 0.92, step = 1;
    const double secPerPx = 1 / pps() * c.speed;
    // A ramped clip's footage doesn't run evenly across it: its grid starts at the clip.
    const double origin = c.isRamped() ? r.left() : r.left() - c.sourceStart / secPerPx;
    // Worked out every few points across what's shown, and read in between.
    const double every = 4, from = visible.left() - 16;
    std::vector<double> table;
    if (c.isRamped())
        for (double xx = from; xx <= visible.right() + 24; xx += every) table.push_back(c.sourceTime(c.start + (xx - r.left()) / pps()));
    auto source = [&](double xx) {
        if (!c.isRamped() || table.empty()) return (xx - origin) * secPerPx;
        if (table.size() < 2) return table[0];
        const double f = (xx - from) / every;
        const int i = std::min(std::max(0, int(f)), int(table.size()) - 2);
        return table[size_t(i)] + (table[size_t(i + 1)] - table[size_t(i)]) * (f - i);
    };
    const std::vector<float>& levels = wf->rms.empty() ? wf->peaks : wf->rms;
    const int count = int(levels.size());
    auto level = [&](long k) {
        const double xx = origin + double(k) * step;
        const int i0 = int(source(xx) * wf->peaksPerSecond);
        const int i1 = int(source(xx + step) * wf->peaksPerSecond);
        if (i0 >= count || i1 < 0) return 0.0;
        const int a = std::max(0, i0), b = std::min(count - 1, std::max(i0, i1 - 1));
        if (a > b) return 0.0;
        const int stride = std::max(1, (b - a) / 32);
        double sum = 0;
        int n = 0;
        for (int i = a; i <= b; i += stride) { const double v = levels[size_t(i)]; sum += v * v; ++n; }
        double v = std::sqrt(sum / std::max(1, n));
        if (wf->rms.empty()) v /= lift;
        return v;
    };
    // A couple of extra samples each side, so the smoothing reaches past the visible edges.
    const long k0 = long(std::floor((visible.left() - origin) / step)) - 2;
    const long k1 = long(std::ceil((visible.right() - origin) / step)) + 2;
    if (k1 <= k0) return;
    std::vector<double> raw;
    raw.reserve(size_t(k1 - k0 + 1));
    for (long k = k0; k <= k1; ++k) raw.push_back(level(k));
    std::vector<double> xs, hs;
    for (size_t j = 2; j + 2 < raw.size(); ++j) {
        // Light smoothing over five samples keeps the edge soft without blurring beats.
        const double v = (raw[j - 2] + raw[j - 1] + raw[j] + raw[j + 1] + raw[j + 2]) / 5;
        xs.push_back(std::min(std::max(origin + double(k0 + long(j)) * step, visible.left()), visible.right()));
        hs.push_back(std::max(0.75, std::min(1.0, v * lift * gain) * room * headroom));
    }
    if (xs.size() < 2) return;
    QPainterPath path;
    path.moveTo(xs[0], base);
    for (size_t i = 0; i < xs.size(); ++i) path.lineTo(xs[i], base - hs[i]);
    path.lineTo(xs.back(), base);
    path.closeSubpath();
    // On green (thumbnails off) the shape is a pale green tint, so it sits in the clip's colour.
    ctx.fillPath(path, c_.model().thumbnails() ? QColor::fromRgbF(0.80f, 0.80f, 0.78f, 0.7f) : QColor::fromRgbF(0.88f, 0.97f, 0.90f, 0.55f));
}

} // namespace er
