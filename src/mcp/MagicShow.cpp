#include "MagicShow.h"
#include "MagicThinking.h"
#include "ui/Controller.h"
#include "ui/Theme.h"
#include "ui/TimelineCanvas.h"
#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <cmath>

namespace er {

namespace {
QPointer<MagicShow> currentShow;
/// Quick at first, settling to an even pace (MagicShow.tempo).
double tempo(double t) { return 0.72 + 1.1 * std::exp(-t / 0.8); }
}

bool MagicShow::animated() { return QSettings().value(QStringLiteral("magicShowWork"), true).toBool(); }
bool MagicShow::running() { return !currentShow.isNull(); }

QColor MagicShow::color(const QString& who) {
    const QString w = who.toLower();
    if (w.contains(QLatin1String("claude"))) return QColor(0xD9, 0x77, 0x57);
    if (w.contains(QLatin1String("chatgpt")) || w.contains(QLatin1String("codex")) || w.contains(QLatin1String("openai"))) return QColor(0x10, 0xA3, 0x7F);
    static const QColor palette[] = {QColor(0x5B, 0x8D, 0xEF), QColor(0xC0, 0x6C, 0xE0), QColor(0xE0, 0xB0, 0x3C), QColor(0x3C, 0xC0, 0xC0)};
    return palette[qHash(w) % 4];
}

void MagicShow::play(Controller& c, const MagicPlan& plan, std::function<void(bool)> done, std::optional<Performer> performer) {
    TimelineCanvas* canvas = TimelineCanvas::current();
    if (!animated() || running() || plan.tasks.empty() || !canvas || !canvas->isVisible() || !canvas->window()->isVisible()) {
        // The edit at once: one undo step, the timeline fitted to it.
        const Project result = plan.result();
        c.model().mutate([&](Project& q) { q = result; });
        if (!performer) c.model().zoomToFit();
        if (done) done(true);
        return;
    }
    currentShow = new MagicShow(c, canvas, plan, std::move(done), std::move(performer));
}

void MagicShow::stopAll() {
    if (auto* t = MagicThinking::current()) t->cancel();
    if (currentShow) currentShow->finish(false);
}

MagicShow::MagicShow(Controller& c, TimelineCanvas* canvas, const MagicPlan& plan, std::function<void(bool)> done, std::optional<Performer> performer)
    : QWidget(canvas->window()), c_(c), canvas_(canvas), plan_(plan), done_(std::move(done)) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setGeometry(parentWidget()->rect());
    doneAt_.assign(plan_.tasks.size(), -1);
    taken_.assign(plan_.tasks.size(), false);
    // The timeline laid out for the finished cut first, so the cursors work where the clips will land.
    EditorModel& m = c_.model();
    if (!performer) {
        const Project result = plan_.result();
        m.live([&](Project& q) { q = result; }, false);
        m.zoomToFit();
    }
    m.live([&](Project& q) { q = plan_.base; }, false);
    // From the Magic button's corner of the window: the swarm (or the agent's one cursor).
    const QPointF spawn(width() * 0.5, height() - 40.0);
    const int n = performer ? 1 : std::clamp(int(plan_.tasks.size() / 4) + 1, 2, 5);
    for (int i = 0; i < n; ++i) {
        Cursor k;
        k.at = spawn + QPointF((i - n / 2) * 18.0, 0);
        k.color = performer ? performer->color : QColor::fromHsvF(0, 0, 0.95);
        k.name = performer ? performer->name : (n > 1 ? QString() : QStringLiteral("Local"));
        cursors_.push_back(k);
    }
    qApp->installEventFilter(this);
    clock_.start();
    connect(&timer_, &QTimer::timeout, this, &MagicShow::step);
    timer_.start(16);
    show();
    raise();
}

MagicShow::~MagicShow() { qApp->removeEventFilter(this); }

bool MagicShow::eventFilter(QObject*, QEvent* e) {
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) { finish(false); return true; }
    if (e->type() == QEvent::Resize && parentWidget()) setGeometry(parentWidget()->rect());
    return false;
}

QPointF MagicShow::target(const MagicTask& task) const {
    const MagicEdit& e = task.edit;
    TimelineCanvas* cv = canvas_.data();
    const Project& now = c_.model().project();
    auto centre = [&](const QRectF& r) { return QPointF(cv->mapTo(parentWidget(), r.center().toPoint())); };
    auto clipNamed = [&](const Uuid& id) -> std::optional<Clip> { if (const Clip* c = now.clip(id)) return *c; return std::nullopt; };
    std::optional<Clip> clip;
    double atTime = -1;
    switch (e.kind) {
    case MagicEdit::Kind::place: if (!e.clips.empty()) clip = e.clips.front(); break;
    case MagicEdit::Kind::move:
        if (!e.ids.empty()) if (auto c = clipNamed(e.ids.front())) {
            clip = *c;
            clip->start += e.to - e.from;
            if (auto it = e.tracks.find(c->id); it != e.tracks.end()) clip->trackID = it->second;
        }
        break;
    case MagicEdit::Kind::trimHead:
    case MagicEdit::Kind::trimTail:
    case MagicEdit::Kind::blade:
        if (!e.ids.empty()) clip = clipNamed(e.ids.front());
        atTime = e.kind == MagicEdit::Kind::blade ? e.at : e.to;
        break;
    case MagicEdit::Kind::remove: if (!e.ids.empty()) clip = clipNamed(e.ids.front()); break;
    default: if (e.clip) clip = clipNamed(*e.clip); break;
    }
    if (clip) if (auto r = cv->rect(*clip)) {
        QRectF rr = *r;
        // Clamped into the canvas, so a cursor never flies off screen.
        rr = rr.intersected(QRectF(cv->QWidget::rect()).adjusted(4, 4, -4, -4));
        if (rr.isEmpty()) rr = QRectF(std::clamp(r->center().x(), 8.0, cv->width() - 8.0), std::clamp(r->center().y(), 8.0, cv->height() - 8.0), 1, 1);
        QPointF p = centre(rr);
        if (atTime >= 0) p.setX(cv->mapTo(parentWidget(), QPoint(int(std::clamp(cv->x(atTime), 4.0, cv->width() - 4.0)), 0)).x());
        return p;
    }
    return QPointF(cv->mapTo(parentWidget(), cv->QWidget::rect().center()));
}

void MagicShow::step() {
    if (!canvas_) { finish(true); return; }
    const double t = now();
    if (endedAt_ >= 0) {
        if (t - endedAt_ > 0.35) {
            timer_.stop();
            hide();
            deleteLater();
        }
        update();
        return;
    }
    const double pace = cursors_.size() == 1 ? 0.85 : tempo(t);
    for (auto& k : cursors_) {
        if (k.task >= 0) {
            const double u = (t - k.t0) / std::max(0.01, k.travel);
            if (!k.clicking) {
                const double s = std::min(1.0, u);
                const double e = s * s * (3 - 2 * s);
                k.at = k.from + (k.to - k.from) * e;
                if (u >= 1) {
                    // There: the edit is made.
                    const MagicEdit& ed = plan_.tasks[size_t(k.task)].edit;
                    c_.model().live([&](Project& q) { ed.apply(q); }, false);
                    k.clicking = true;
                    k.t0 = t;
                }
            } else if (t - k.t0 > 0.12 * pace) {
                doneAt_[size_t(k.task)] = t;
                k.task = -1;
                k.clicking = false;
            }
        }
    }
    // Free cursors take the next tasks that are ready.
    for (auto& k : cursors_) {
        if (k.task >= 0) continue;
        for (size_t i = 0; i < plan_.tasks.size(); ++i) {
            if (taken_[i]) continue;
            const MagicTask& task = plan_.tasks[i];
            double readyAt = 0;
            bool ready = true;
            for (int a : task.after) {
                if (a < 0 || size_t(a) >= doneAt_.size()) continue;
                if (doneAt_[size_t(a)] < 0) { ready = false; break; }
                readyAt = std::max(readyAt, doneAt_[size_t(a)]);
            }
            if (!ready || t < readyAt + task.delay * pace) continue;
            taken_[i] = true;
            k.task = int(i);
            k.from = k.at;
            k.to = target(task);
            const double dist = std::hypot(k.to.x() - k.from.x(), k.to.y() - k.from.y());
            k.travel = std::clamp(dist / 1600.0, 0.16, 0.55) * pace;
            k.t0 = t;
            break;
        }
    }
    if (std::all_of(doneAt_.begin(), doneAt_.end(), [](double d) { return d >= 0; })) finish(true);
    update();
}

void MagicShow::finish(bool finished) {
    if (endedAt_ >= 0) return;
    endedAt_ = now();
    qApp->removeEventFilter(this);
    EditorModel& m = c_.model();
    if (finished) m.commitLive(); else m.discardLive();
    if (!timer_.isActive()) timer_.start(16);
    auto done = std::move(done_);
    if (done) done(finished);
}

void MagicShow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double fade = endedAt_ >= 0 ? std::max(0.0, 1 - (now() - endedAt_) / 0.35) : 1.0;
    p.setOpacity(fade);
    for (const auto& k : cursors_) {
        // An arrow in the cursor's colour, outlined, with its name beside it.
        QPainterPath arrow;
        const QPointF o = k.at;
        arrow.moveTo(o);
        arrow.lineTo(o + QPointF(0, 17));
        arrow.lineTo(o + QPointF(4.5, 13));
        arrow.lineTo(o + QPointF(8, 20));
        arrow.lineTo(o + QPointF(10.5, 19));
        arrow.lineTo(o + QPointF(7, 12));
        arrow.lineTo(o + QPointF(12.5, 12));
        arrow.closeSubpath();
        const double press = k.clicking ? 0.85 : 1.0;
        p.save();
        p.translate(o);
        p.scale(press, press);
        p.translate(-o);
        p.setPen(QPen(k.color.lightness() > 180 ? QColor(20, 20, 20) : QColor(255, 255, 255), 1.2));
        p.setBrush(k.color);
        p.drawPath(arrow);
        p.restore();
        if (!k.name.isEmpty()) {
            p.setFont(Theme::font(11, 600));
            const QRectF tag = QFontMetricsF(p.font()).boundingRect(k.name).adjusted(-6, -3, 6, 3).translated(o + QPointF(16, 26));
            p.setPen(Qt::NoPen);
            p.setBrush(k.color);
            p.drawRoundedRect(tag, 6, 6);
            p.setPen(k.color.lightness() > 180 ? QColor(20, 20, 20) : QColor(255, 255, 255));
            p.drawText(tag, Qt::AlignCenter, k.name);
        }
    }
}

} // namespace er
