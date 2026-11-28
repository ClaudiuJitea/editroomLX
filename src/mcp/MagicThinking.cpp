#include "MagicThinking.h"
#include "MagicShow.h"
#include "ui/Controller.h"
#include "ui/LibraryPanel.h"
#include "ui/PreviewPanel.h"
#include "ui/Theme.h"
#include "ui/TimelineCanvas.h"
#include <QAbstractButton>
#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <cmath>

namespace er {

namespace {
QPointer<MagicThinking> showing, leaving;
constexpr double dimming = 0.45, ghostDraw = 0.7, ghostLeave = 0.3, fadeTime = 0.6;

QPainterPath brackets(const QRectF& r, double length) {
    const double o = 1.5;
    const double x0 = r.left() - o, y0 = r.top() - o, x1 = r.right() + o, y1 = r.bottom() + o;
    const double l = std::min({length, r.width() / 3, r.height() / 3});
    QPainterPath p;
    p.moveTo(x0, y0 + l); p.lineTo(x0, y0); p.lineTo(x0 + l, y0);
    p.moveTo(x1 - l, y0); p.lineTo(x1, y0); p.lineTo(x1, y0 + l);
    p.moveTo(x1, y1 - l); p.lineTo(x1, y1); p.lineTo(x1 - l, y1);
    p.moveTo(x0 + l, y1); p.lineTo(x0, y1); p.lineTo(x0, y1 - l);
    return p;
}

double noise(double a, double b) { return std::sin(a * 1.7 + std::sin(b * 1.3)) * 0.5 + std::sin(b * 2.1 - a * 0.6) * 0.3 + std::sin((a + b) * 0.9) * 0.2; }

/// The part of `r` inside `area`, when at least `share` of it is.
std::optional<QRectF> mostlyIn(const QRectF& r, const QRectF& area, double share = 0.6) {
    const QRectF v = r.intersected(area);
    if (v.isEmpty() || r.width() * r.height() <= 0 || v.width() * v.height() < share * r.width() * r.height() || v.width() <= 6 || v.height() <= 6)
        return std::nullopt;
    return v;
}
} // namespace

QString MagicThinking::word(Stage s) {
    switch (s) {
    case Stage::looking: return QStringLiteral("Looking");
    case Stage::analyzing: return QStringLiteral("Analyzing");
    case Stage::thinking: return QStringLiteral("Thinking");
    }
    return {};
}

MagicThinking* MagicThinking::current() { return showing.data(); }

MagicThinking* MagicThinking::begin(Controller& c, const QString& name, const QColor& color) {
    if (showing) showing->cancel();
    if (leaving) leaving->cancel();
    if (!MagicShow::animated()) return nullptr;
    TimelineCanvas* canvas = TimelineCanvas::current();
    if (!canvas || !canvas->isVisible() || !canvas->window()->isVisible()) return nullptr;
    showing = new MagicThinking(c, canvas->window(), name, color);
    return showing.data();
}

MagicThinking::MagicThinking(Controller& c, QWidget* window, const QString& name, const QColor& color)
    : QWidget(window), c_(c), color_(color), label_(name) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setGeometry(window->rect());
    clock_.start();
    // The viewer's picture, if it's presented straight from the GPU, is painted for now, so the scene shows on it.
    setViewerOverlay(true);
    qApp->installEventFilter(this);
    connect(&timer_, &QTimer::timeout, this, &MagicThinking::step);
    timer_.start(16);
    show();
    raise();
    look();
}

MagicThinking::~MagicThinking() { qApp->removeEventFilter(this); }

void MagicThinking::setViewerOverlay(bool on) {
    QWidget* w = parentWidget();
    if (!w) return;
    w->setProperty("magicOverlay", on);
    for (auto* v : w->findChildren<Viewer*>()) v->updatePresenting();
}

void MagicThinking::cancel() {
    timer_.stop();
    qApp->removeEventFilter(this);
    setViewerOverlay(false);
    hide();
    if (showing == this) showing = nullptr;
    if (leaving == this) leaving = nullptr;
    deleteLater();
}

void MagicThinking::end(std::function<void()> done) {
    if (leaveDone_ || fadeFrom_) return;
    fadeFrom_ = now();
    leaveDone_ = std::move(done);
    if (!leaveDone_) qApp->removeEventFilter(this);
}

void MagicThinking::handOver() {
    if (leaveDone_ || fadeFrom_) return;
    fadeFrom_ = now();
    handedOver_ = true;
    qApp->removeEventFilter(this);
    if (showing == this) { showing = nullptr; leaving = this; }
}

double MagicThinking::secondsUntilDrawn() const {
    double newest = now();
    if (!ghosts_.empty()) { newest = -1e9; for (const auto& g : ghosts_) newest = std::max(newest, g.born); }
    return std::max(0.0, newest + ghostDraw - now());
}

void MagicThinking::advance(Stage s) {
    lastEvent_ = now();
    if (fadeFrom_ || int(s) <= int(stage_)) return;
    stage_ = s;
    emit stageChanged(s);
}

QRectF MagicThinking::viewerRect() const {
    if (auto* v = parentWidget()->findChild<Viewer*>(); v && v->isVisible())
        return QRectF(v->mapTo(parentWidget(), v->picture().topLeft().toPoint()), v->picture().size());
    return QRectF(300, 80, std::max(200, width() - 640), 360);
}

std::optional<QRectF> MagicThinking::buttonRect() const {
    for (auto* b : parentWidget()->findChildren<QAbstractButton*>(QStringLiteral("magicButton")))
        if (b->isVisible()) return QRectF(b->mapTo(parentWidget(), QPoint(0, 0)), b->size());
    return std::nullopt;
}

std::optional<QRectF> MagicThinking::mediaRect(const Uuid& asset) const {
    for (auto* grid : parentWidget()->findChildren<MediaGrid*>()) {
        if (!grid->isVisible()) continue;
        const QRectF seen = grid->visibleRegion().boundingRect();
        for (const auto& it : grid->items()) {
            if (it.folder || it.asset.id != asset) continue;
            // Any part on screen is scanned, cut off at the list's edge like the picture is.
            const QRectF r = it.picture.isEmpty() ? it.rect : it.picture;
            auto v = mostlyIn(r, seen, 0);
            if (!v) return std::nullopt;
            return QRectF(grid->mapTo(parentWidget(), v->topLeft().toPoint()), v->size());
        }
    }
    return std::nullopt;
}

std::vector<QRectF> MagicThinking::mediaRects(const std::optional<std::set<Uuid>>& assets) const {
    std::vector<QRectF> out;
    for (const auto& a : c_.model().project().assets)
        if (!assets || assets->count(a.id)) if (auto r = mediaRect(a.id)) out.push_back(*r);
    return out;
}

std::optional<QRectF> MagicThinking::clipRect(const Clip& c, const Project& project) const {
    TimelineCanvas* cv = TimelineCanvas::current();
    if (!cv) return std::nullopt;
    // Laid out as the project the clip is in (the coming edit's, with any tracks it adds), as the Mac's magicWindowRect.
    auto r = cv->rect(c, project);
    if (!r) return std::nullopt;
    return QRectF(r->topLeft() + QPointF(cv->mapTo(parentWidget(), QPoint(0, 0))), r->size());
}

void MagicThinking::look() {
    lastEvent_ = now();
    if (stage_ != Stage::looking) return;
    targets_ = mediaRects(std::nullopt);
}

void MagicThinking::analyze(const std::optional<std::set<Uuid>>& assets) {
    advance(Stage::analyzing);
    if (stage_ != Stage::analyzing) return;
    std::vector<QRectF> rects;
    if (TimelineCanvas* cv = TimelineCanvas::current()) {
        const QRectF visible(cv->mapTo(parentWidget(), QPoint(0, 0)), cv->size());
        for (const auto& c : c_.model().project().clips) {
            // Every title and sound, and the clips of the items being studied.
            if (c.assetID && assets && (c.kind == ClipKind::video || c.kind == ClipKind::image) && !assets->count(*c.assetID)) continue;
            if (auto r = clipRect(c, c_.model().project()); r && visible.intersects(*r)) rects.push_back(r->intersected(visible));
        }
    }
    for (const auto& r : mediaRects(assets)) rects.push_back(r);
    rects.push_back(viewerRect());
    targets_.clear();
    for (const auto& r : rects) if (r.width() > 6 && r.height() > 6) targets_.push_back(r);
}

void MagicThinking::focus(const Uuid& asset) {
    if (int(stage_) < int(Stage::analyzing)) analyze(std::nullopt);
    lastEvent_ = now();
    if (fadeFrom_) return;
    std::optional<QRectF> rect;
    if (TimelineCanvas* cv = TimelineCanvas::current())
        for (const auto& c : c_.model().project().clips)
            if (c.assetID == asset) {
                if (auto r = clipRect(c, c_.model().project())) rect = mostlyIn(*r, QRectF(cv->mapTo(parentWidget(), QPoint(0, 0)), cv->size()));
                break;
            }
    if (!rect) rect = mediaRect(asset);
    if (!rect) return;
    if (stage_ == Stage::thinking) addScan(*rect); else focusQueue_.push_back(*rect);
}

void MagicThinking::think() { advance(Stage::thinking); }

void MagicThinking::plan(const Project& project) {
    advance(Stage::thinking);
    if (fadeFrom_ || !TimelineCanvas::current()) return;
    // Every clip, a video's own sound included; that sound is drawn at the same moment as its video.
    auto linkedSound = [](const Clip& c) { return c.kind == ClipKind::audio && c.linkID; };
    std::vector<Clip> clips;
    for (const auto& c : project.clips) if (!linkedSound(c)) clips.push_back(c);
    for (const auto& c : project.clips) if (linkedSound(c)) clips.push_back(c);
    for (const auto& c : clips) {
        const QString key = QStringLiteral("%1|%2|%3").arg(c.trackID.toString()).arg(int(c.start * 100)).arg(int(c.duration * 100));
        if (std::find(ghostKeys_.begin(), ghostKeys_.end(), key) != ghostKeys_.end()) continue;
        auto r = clipRect(c, project);
        if (!r || r->width() <= 3) continue;
        ghostKeys_.push_back(key);
        double newest = 0;
        for (const auto& g : ghosts_) newest = std::max(newest, g.born);
        std::optional<double> born;
        if (linkedSound(c)) if (auto it = linkBorn_.find(*c.linkID); it != linkBorn_.end()) born = it->second;
        Ghost g{c, project, born.value_or(std::max(now(), newest + 0.12)), std::nullopt};
        if (!linkedSound(c) && c.linkID) linkBorn_[*c.linkID] = g.born;
        ghosts_.push_back(g);
    }
}

void MagicThinking::addScan(const QRectF& r) {
    const double t = now();
    const double sweep = (1.0 + 0.5 * QRandomGenerator::global()->generateDouble()) * std::min(1.6, std::max(0.8, r.width() / 220));
    scans_.push_back({r, t, sweep});
    lastScan_ = t;
}

bool MagicThinking::eventFilter(QObject* o, QEvent* e) {
    // MagicLock: the editor's window is locked while Magic works it out.
    auto* w = qobject_cast<QWidget*>(o);
    if (!w || w->window() != parentWidget()) return false;
    switch (e->type()) {
    case QEvent::KeyPress: {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape) { MagicShow::stopAll(); if (showing == this) cancel(); return true; }
        if (k->key() == Qt::Key_Q && k->modifiers() == Qt::ControlModifier) return false;
        if (k->key() == Qt::Key_Space && k->modifiers() == Qt::NoModifier) return false;
        nudged_ = now();
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        // The Magic button (Stop) still works.
        if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->objectName() == QLatin1String("magicButton")) return false;
        nudged_ = now();
        return true;
    }
    case QEvent::Wheel:
    case QEvent::NativeGesture:
        return true;
    default: return false;
    }
}

void MagicThinking::step() {
    if (!parentWidget()) return cancel();
    if (geometry() != parentWidget()->rect()) setGeometry(parentWidget()->rect());
    const double t = now();
    const double dt = std::clamp(t - last_, 1.0 / 240, 1.0 / 30);
    last_ = t;
    // Analyzing, then a while with nothing new: it's thinking.
    if (!fadeFrom_ && stage_ == Stage::analyzing && t - lastEvent_ > 2.5 && focusQueue_.empty()) think();
    const double fade = fadeFrom_ ? std::max(0.0, 1 - (t - *fadeFrom_) / fadeTime) : 1.0;
    // Scans: a few at a time, wanted ones first; none new once it's thinking.
    int busy = 0;
    for (const auto& s : scans_) if (t < s.start + s.sweep) ++busy;
    const int room = std::min(6, std::max(1, int(targets_.size()) / 2 + 1));
    if (!fadeFrom_ && stage_ != Stage::thinking && busy < room && t - lastScan_ > 0.18 && t > 0.25) {
        if (!focusQueue_.empty()) {
            addScan(focusQueue_.front());
            focusQueue_.erase(focusQueue_.begin());
        } else if (!targets_.empty()) {
            std::vector<QRectF> fresh, pool;
            auto active = [&](const QRectF& r) { return std::any_of(scans_.begin(), scans_.end(), [&](const Scan& s) { return s.rect == r; }); };
            for (const auto& r : targets_) if (!active(r) && std::find(recent_.begin(), recent_.end(), r) == recent_.end()) fresh.push_back(r);
            if (fresh.empty()) for (const auto& r : targets_) if (!active(r)) pool.push_back(r);
            const auto& from = fresh.empty() ? pool : fresh;
            if (!from.empty()) {
                const QRectF r = from[size_t(QRandomGenerator::global()->bounded(int(from.size())))];
                addScan(r);
                recent_.push_back(r);
                if (int(recent_.size()) > std::max(1, int(targets_.size()) * 2 / 3)) recent_.erase(recent_.begin());
            }
        }
    }
    std::erase_if(scans_, [&](const Scan& s) { return t > s.end(); });
    const double want = stage_ == Stage::thinking ? 1.0 : 0.0;
    fieldAlpha_ += (want - fieldAlpha_) * std::min(1.0, dt * (want > fieldAlpha_ ? 1.2 : 3));
    // Outlines go as their clips land (after the handover), or all when the show stops.
    if (handedOver_) {
        const auto& clips = c_.model().project().clips;
        for (auto& g : ghosts_) {
            if (g.landed) continue;
            const bool there = std::any_of(clips.begin(), clips.end(), [&](const Clip& c) {
                return c.trackID == g.clip.trackID && std::abs(c.start - g.clip.start) < 0.02 && std::abs(c.duration - g.clip.duration) < 0.02;
            });
            if (there || !MagicShow::running()) g.landed = t;
        }
        if (fadeFrom_ && t - *fadeFrom_ >= fadeTime
            && std::all_of(ghosts_.begin(), ghosts_.end(), [&](const Ghost& g) { return g.landed && *g.landed + ghostLeave <= t; })) {
            cancel();
            return;
        }
        if (fadeFrom_ && t - *fadeFrom_ >= fadeTime) setViewerOverlay(false);
    } else if (fadeFrom_ && t - *fadeFrom_ >= fadeTime) {
        auto done = std::move(leaveDone_);
        cancel();
        if (done) done();
        return;
    }
    update();
}

void MagicThinking::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double t = now();
    const double fade = fadeFrom_ ? std::max(0.0, 1 - (t - *fadeFrom_) / fadeTime) : 1.0;
    const double appear = std::min(1.0, t / 0.35);
    // The app dims, all but the Magic button (Stop).
    QPainterPath dim;
    dim.addRect(QRectF(rect()));
    if (auto b = buttonRect()) {
        QPainterPath hole;
        const QRectF r = b->adjusted(-3, -3, 3, 3);
        hole.addRoundedRect(r, r.height() / 2, r.height() / 2);
        dim = dim.subtracted(hole);
    }
    p.fillPath(dim, QColor(0, 0, 0, int(255 * dimming * appear * fade)));
    const QRectF view = viewerRect();
    // The dot field over the viewer while thinking.
    if (fieldAlpha_ > 0.01 && view.width() > 10 && view.height() > 10) {
        p.save();
        p.setOpacity(fieldAlpha_ * fade);
        p.setClipRect(view);
        p.fillRect(view, QColor(0, 0, 0, 56));
        const double w = view.width(), h = view.height();
        const double spacing = std::max(6.0, std::min(w, h) / 55);
        const QColor warm = QColor::fromRgbF(color_.redF() * 0.45 + 0.55, color_.greenF() * 0.45 + 0.55, color_.blueF() * 0.45 + 0.55);
        p.setPen(Qt::NoPen);
        for (double y = spacing / 2; y < h; y += spacing)
            for (double x = spacing / 2; x < w; x += spacing) {
                const double u = x / w, v = y / h;
                const double level = std::min(1.0, std::pow(noise(u * 4 + t * 0.6, v * 3.2 - t * 0.44) * 0.5 + 0.5, 2.6) * 1.2) * 0.65;
                const double radius = 0.35 + level * spacing * 0.16;
                p.setBrush(QColor::fromRgbF(warm.redF() + (1 - warm.redF()) * level * 0.5, warm.greenF() + (1 - warm.greenF()) * level * 0.4,
                                            warm.blueF() + (1 - warm.blueF()) * level * 0.3, 0.05 + 0.6 * level));
                p.drawEllipse(QPointF(view.left() + x, view.top() + y), radius, radius);
            }
        p.restore();
    }
    // The viewer's own brackets.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, int(191 * appear * fade)), 1));
    p.drawPath(brackets(view, 6));
    // Scans: a fill sweeping across with a bright edge, brackets on the corners.
    for (const auto& s : scans_) {
        const double u = (t - s.start) / s.sweep;
        const QRectF r = s.rect;
        const double width = r.width() * std::clamp(u, 0.0, 1.0);
        const double after = std::max(0.0, t - s.start - s.sweep);
        const double sweepAlpha = u < 1 ? 1 : std::max(0.0, 1 - after / 0.3);
        p.fillRect(QRectF(r.left(), r.top(), width, r.height()), QColor(255, 255, 255, int(18 * sweepAlpha * fade)));
        p.fillRect(QRectF(r.left() + width - 0.75, r.top(), 1.5, r.height()), QColor(255, 255, 255, int(204 * sweepAlpha * fade)));
        const double bracketIn = std::min(1.0, (t - s.start) / 0.15);
        const double bracketOut = std::clamp(1 - (after - 0.25) / 0.3, 0.0, 1.0);
        p.setPen(QPen(QColor(255, 255, 255, int(191 * bracketIn * bracketOut * fade)), 1));
        p.drawPath(brackets(r, r == view ? 6 : 3));
    }
    // Outlines of the coming edit, drawing themselves, dashes crawling.
    for (const auto& g : ghosts_) {
        auto rr = clipRect(g.clip, g.project);
        if (!rr) continue;
        const QRectF r = rr->adjusted(1, 3, -1, -3);
        const double progress = std::clamp((t - g.born) / ghostDraw, 0.0, 1.0);
        const double alpha = handedOver_ ? std::max(0.0, 1 - (t - g.landed.value_or(t)) / ghostLeave) : fade;
        if (progress <= 0 || alpha <= 0) continue;
        QPainterPath outline;
        const double radius = std::min({6.0, r.width() / 2, r.height() / 2});
        outline.addRoundedRect(r, radius, radius);
        p.fillPath(outline, QColor(255, 255, 255, int(15 * std::max(0.0, (progress - 0.6) / 0.4) * alpha)));
        QPen pen(QColor(255, 255, 255, int(166 * alpha)), 1.5);
        pen.setDashPattern({7.0 / 1.5, 6.0 / 1.5});
        pen.setDashOffset(-t * 24 / 1.5);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        // Drawn round as far as it's got.
        const double length = outline.length();
        QPainterPath part;
        const int steps = 64;
        const int n = int(steps * progress);
        for (int i = 0; i <= n; ++i) {
            const QPointF q = outline.pointAtPercent(std::min(1.0, i / double(steps)));
            if (i == 0) part.moveTo(q); else part.lineTo(q);
        }
        (void)length;
        p.drawPath(part);
    }
    // "Press esc to cancel", small, at the bottom; a blocked click makes it swell a touch.
    const double bump = std::max(0.0, 1 - (t - nudged_) / 0.45);
    const double swell = std::sin(std::min(1.0, (t - nudged_) / 0.45) * M_PI) * bump;
    const double hintAlpha = std::max(std::min(1.0, std::max(0.0, t - 0.6) / 0.4), bump) * fade;
    if (hintAlpha > 0 && !handedOver_) {
        p.save();
        p.translate(width() / 2.0, height() - 26.0);
        p.scale(1 + 0.08 * swell, 1 + 0.08 * swell);
        const QRectF pill(-70, -11, 140, 22);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, int(178 * hintAlpha)));
        p.drawRoundedRect(pill, 11, 11);
        p.setFont(Theme::font(11, 500));
        p.setPen(QColor(255, 255, 255, int(255 * (0.75 + 0.25 * bump) * hintAlpha)));
        p.drawText(pill, Qt::AlignCenter, QStringLiteral("Press esc to cancel"));
        p.restore();
    }
}

} // namespace er
