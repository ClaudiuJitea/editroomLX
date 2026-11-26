#include "TimelinePanel.h"
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QCoreApplication>
#include "ExportDialog.h"
#include "LevelMeter.h"
#include "Symbols.h"
#include "Theme.h"
#include "Widgets.h"
#include "model/Timecode.h"
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <cmath>

namespace er {

namespace {

/// A button drawn by a function, with hover tracked.
class Painted : public QAbstractButton {
public:
    using Paint = std::function<void(QPainter&, const QRectF&, bool hovering, bool enabled)>;
    Painted(QSize size, Paint paint, QWidget* parent = nullptr) : QAbstractButton(parent), paint_(std::move(paint)) {
        setFixedSize(size);
        setCursor(Qt::ArrowCursor);
    }
    void setPaint(Paint p) { paint_ = std::move(p); update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paint_(p, QRectF(rect()), underMouse(), isEnabled());
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    Paint paint_;
};

QColor alpha(QColor c, double a) { c.setAlphaF(float(c.alphaF() * a)); return c; }

void roundRect(QPainter& p, const QRectF& r, double radius, const QColor& fill, const QColor& stroke = Qt::transparent) {
    p.setPen(stroke.alpha() ? QPen(stroke, 1) : Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(stroke.alpha() ? r.adjusted(0.5, 0.5, -0.5, -0.5) : r, radius, radius);
}

// MARK: Toolbar pieces

/// One key of the tool selector.
Painted* toolKey(const QString& symbol, const QString& help, std::function<bool()> on, std::function<void()> action) {
    auto* b = new Painted(QSize(30, 24), [symbol, on](QPainter& p, const QRectF& r, bool hovering, bool) {
        const bool o = on();
        if (o) roundRect(p, r, 6, Theme::raised(), Theme::ink(0.1));
        Symbols::draw(p, symbol, r.center(), 12, o || hovering ? Theme::text() : Theme::textDim(), 500);
    });
    b->setToolTip(help);
    QObject::connect(b, &QAbstractButton::clicked, b, [action] { action(); });
    return b;
}

/// A plain text command button (Split, Delete).
Painted* textAction(const QString& title, const QString& help, std::function<void()> action) {
    const QFont f = Theme::font(11.5, 500);
    const int w = int(std::ceil(QFontMetricsF(f).horizontalAdvance(title))) + 18;
    auto* b = new Painted(QSize(w, 24), [title, f](QPainter& p, const QRectF& r, bool hovering, bool enabled) {
        if (hovering && enabled) roundRect(p, r, 7, Theme::hover());
        p.setOpacity(enabled ? 1 : 0.4);
        p.setFont(f);
        p.setPen(hovering && enabled ? Theme::text() : Theme::textDim());
        p.drawText(r, Qt::AlignCenter, title);
    });
    b->setToolTip(help);
    QObject::connect(b, &QAbstractButton::clicked, b, [action] { action(); });
    return b;
}

/// A labelled on/off toggle. On is the same grey as the Split and Delete commands; off matches a disabled
/// Delete (0.2). Neither has a key behind it, only a faint one on hover.
Painted* toggleChip(const QString& title, const QString& help, std::function<bool()> on, std::function<void()> action) {
    const QFont f = Theme::font(11.5, 500);
    const int w = int(std::ceil(QFontMetricsF(f).horizontalAdvance(title))) + 20;
    auto* b = new Painted(QSize(w, 24), [title, f, on](QPainter& p, const QRectF& r, bool hovering, bool) {
        if (hovering) roundRect(p, r, 7, Theme::hover());
        p.setFont(f);
        p.setPen(on() ? (hovering ? Theme::text() : Theme::textDim()) : alpha(Theme::textDim(), hovering ? 0.6 : 0.2));
        p.drawText(r, Qt::AlignCenter, title);
    });
    b->setToolTip(help);
    QObject::connect(b, &QAbstractButton::clicked, b, [action] { action(); });
    return b;
}

Painted* iconButton(const QString& symbol, const QString& help, std::function<void()> action, int size = 28) {
    auto* b = new Painted(QSize(size, size), [symbol, size](QPainter& p, const QRectF& r, bool hovering, bool enabled) {
        if (hovering) roundRect(p, r, 7, Theme::hover());
        p.setOpacity(enabled ? 1 : 0.35);
        Symbols::draw(p, symbol, r.center(), size * 0.46, hovering ? Theme::text() : Theme::textDim(), 500);
    });
    b->setToolTip(help);
    QObject::connect(b, &QAbstractButton::clicked, b, [action] { action(); });
    return b;
}

} // namespace

/// TimelineToolbar: left, tools and the edits they go with; right, editing modes and zoom.
class TimelineToolbar : public QWidget {
public:
    TimelineToolbar(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
        setFixedHeight(44);
        EditorModel& m = c_.model();
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(12, 0, 12, 0);
        h->setSpacing(8);
        auto* tools = new QWidget;
        tools->setObjectName(QStringLiteral("toolKeys"));
        tools->setAttribute(Qt::WA_StyledBackground);
        tools->setStyleSheet(QStringLiteral("#toolKeys { background: %1; border-radius: 9px; }").arg(Theme::bg().name()));
        auto* th = new QHBoxLayout(tools);
        th->setContentsMargins(3, 3, 3, 3);
        th->setSpacing(2);
        th->addWidget(toolKey(QStringLiteral("cursorarrow"), QStringLiteral("Select (V or A)"), [&m] { return m.tool == Tool::move; },
                              [this, &m] { m.tool = Tool::move; refresh(); }));
        th->addWidget(toolKey(QStringLiteral("scissors"), QStringLiteral("Cut (C)"), [&m] { return m.tool == Tool::blade; },
                              [this, &m] { m.tool = Tool::blade; refresh(); }));
        h->addWidget(tools);
        h->addWidget(textAction(QStringLiteral("Split"), QStringLiteral("Split at Playhead (Ctrl+K)"), [&m] { m.splitAtPlayhead(); }));
        delete_ = textAction(QStringLiteral("Delete"), QStringLiteral("Delete (Backspace)"), [&m] { m.deleteSelected(); });
        h->addWidget(delete_);
        range_ = new QWidget;
        buildRange();
        h->addWidget(range_);
        h->addStretch(1);
        auto chip = [&](const QString& t, const QString& help, std::function<bool()> on, std::function<void()> act) {
            h->addWidget(toggleChip(t, help, on, [this, act] { act(); refresh(); }));
        };
        chip(QStringLiteral("Strip"), QStringLiteral("Filmstrip: pictures in clips. Off shows clips as flat colour by type"),
             [&m] { return m.thumbnails(); }, [&m] { m.setThumbnails(!m.thumbnails()); });
        chip(QStringLiteral("Snap"), QStringLiteral("Snapping (S)"), [&m] { return m.snapping; }, [&m] { m.snapping = !m.snapping; });
        chip(QStringLiteral("Sticky"), QStringLiteral("Sticky Edges (R): clips touching a clip that changes length move with it"),
             [&m] { return m.sticky(); }, [&m] { m.setSticky(!m.sticky()); });
        chip(QStringLiteral("Skim"), QStringLiteral("Skimming (K): hover the timeline to preview frames"),
             [&m] { return m.skimming(); }, [&m] { m.setSkimming(!m.skimming()); });
        chip(QStringLiteral("Audio"), QStringLiteral("Audio Scrubbing (Shift+S): hear sound while dragging the playhead or skimming"),
             [&m] { return m.audioScrub(); }, [&m] { m.setAudioScrub(!m.audioScrub()); });
        h->addSpacing(4);
        h->addWidget(iconButton(QStringLiteral("arrow.left.and.right"), QStringLiteral("Zoom to Fit (Shift+Z)"), [&m] { m.zoomToFit(); }));
        connect(&m, &EditorModel::changed, this, &TimelineToolbar::refresh);
        connect(&m, &EditorModel::selectionChanged, this, &TimelineToolbar::refresh);
        connect(&m, &EditorModel::playheadChanged, this, [this] { if (range_->isVisible()) range_->update(); });
        refresh();
    }
    void refresh() {
        const EditorModel& m = c_.model();
        delete_->setEnabled(!(m.selection().empty() && m.mediaSelection.empty() && m.folderSelection.empty()));
        range_->setVisible(m.project().inPoint || m.project().outPoint);
        for (auto* w : findChildren<QWidget*>()) w->update();
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Theme::panel());
        // panelTopLight: a faint highlight along the top edge, like light catching the lip of a housing.
        p.fillRect(QRectF(0, 0, width(), 1), Theme::ink(0.045));
    }
private:
    /// RangeReadout: the in/out range; click In or Out to go there, ✕ to clear it.
    void buildRange() {
        EditorModel& m = c_.model();
        auto* outer = new QHBoxLayout(range_);
        outer->setContentsMargins(4, 0, 0, 0);
        outer->setSpacing(0);
        auto* box = new QWidget;
        box->setObjectName(QStringLiteral("rangeReadout"));
        box->setAttribute(Qt::WA_StyledBackground);
        box->setStyleSheet(QStringLiteral("#rangeReadout { background: %1; border-radius: 7px; }").arg(Theme::bg().name()));
        box->setFixedHeight(24);
        auto* h = new QHBoxLayout(box);
        h->setContentsMargins(10, 0, 4, 0);
        h->setSpacing(6);
        auto point = [&](const QString& label) {
            const bool isIn = label == QStringLiteral("In");
            auto* b = new Painted(QSize(10, 24), {});
            auto paint = [this, label, isIn, b](QPainter& p, const QRectF& r, bool, bool) {
                const Project& proj = c_.model().project();
                const double t = isIn ? proj.inPoint.value_or(0) : proj.outPoint.value_or(proj.duration());
                const QFont lf = Theme::font(11, 500), tf = Theme::mono(11, 500);
                const QString time = Timecode::string(t, proj.fps);
                const double lw = QFontMetricsF(lf).horizontalAdvance(label), tw = QFontMetricsF(tf).horizontalAdvance(time);
                const int need = int(std::ceil(lw + 5 + tw));
                if (b->width() != need) QMetaObject::invokeMethod(b, [b, need] { b->setFixedWidth(need); }, Qt::QueuedConnection);
                p.setFont(lf);
                p.setPen(Theme::textFaint());
                p.drawText(QRectF(0, 0, lw, r.height()), Qt::AlignVCenter | Qt::AlignLeft, label);
                p.setFont(tf);
                p.setPen(Theme::text());
                p.drawText(QRectF(lw + 5, 0, tw + 1, r.height()), Qt::AlignVCenter | Qt::AlignLeft, time);
            };
            b->setPaint(paint);
            b->setToolTip(QStringLiteral("Go to %1 (%2 sets it)").arg(label, isIn ? QStringLiteral("I") : QStringLiteral("O")));
            connect(b, &QAbstractButton::clicked, this, [this, isIn] {
                const Project& proj = c_.model().project();
                c_.model().seek(isIn ? proj.inPoint.value_or(0) : proj.outPoint.value_or(proj.duration()));
            });
            h->addWidget(b);
        };
        point(QStringLiteral("In"));
        point(QStringLiteral("Out"));
        auto* length = new Painted(QSize(10, 24), {});
        length->setPaint([this, length](QPainter& p, const QRectF& r, bool, bool) {
            auto range = c_.model().project().playRange();
            if (!range) return;
            const QString s = QStringLiteral("· ") + ExportDialog::rangeLength(range->end - range->start);
            const QFont f = Theme::mono(11, 500);
            const int need = int(std::ceil(QFontMetricsF(f).horizontalAdvance(s)));
            if (length->width() != need) QMetaObject::invokeMethod(length, [length, need] { length->setFixedWidth(need); }, Qt::QueuedConnection);
            p.setFont(f);
            p.setPen(Theme::textFaint());
            p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, s);
        });
        length->setAttribute(Qt::WA_TransparentForMouseEvents);
        h->addWidget(length);
        auto* clear = new Painted(QSize(16, 16), [](QPainter& p, const QRectF& r, bool hovering, bool) {
            p.setFont(Theme::font(10, 600));
            p.setPen(hovering ? Theme::text() : Theme::textFaint());
            p.drawText(r, Qt::AlignCenter, QStringLiteral("✕"));
        });
        clear->setToolTip(QStringLiteral("Clear In and Out (Shift+X)"));
        connect(clear, &QAbstractButton::clicked, this, [&m] { m.clearRange(); });
        h->addWidget(clear);
        outer->addWidget(box);
    }
    Controller& c_;
    Painted* delete_;
    QWidget* range_;
};

// MARK: Track headers

namespace {

/// A small square key with an icon, for hide and lock: white when on.
Painted* miniKey(std::function<QString()> symbol, std::function<bool()> on, std::function<QString()> help, std::function<void()> action) {
    auto* b = new Painted(QSize(20, 20), [symbol, on](QPainter& p, const QRectF& r, bool hovering, bool) {
        const bool o = on();
        roundRect(p, r, 5, o ? Theme::text() : (hovering ? Theme::hover() : Theme::field()));
        Symbols::draw(p, symbol(), r.center(), 9.5, o ? alpha(Theme::onText(), 0.85) : (hovering ? Theme::text() : Theme::textDim()), 600);
    });
    b->setToolTip(help());
    QObject::connect(b, &QAbstractButton::clicked, b, [action] { action(); });
    return b;
}

/// A one-letter toggle, for mute and solo.
Painted* letterToggle(const QString& letter, std::function<bool()> on, const QString& help, std::function<void()> action) {
    auto* b = new Painted(QSize(20, 20), [letter, on](QPainter& p, const QRectF& r, bool hovering, bool) {
        const bool o = on();
        roundRect(p, r, 5, o ? Theme::text() : (hovering ? Theme::hover() : Theme::field()));
        p.setFont(Theme::font(10, 700));
        p.setPen(o ? QColor::fromRgbF(0, 0, 0, 0.85f) : (hovering ? Theme::text() : Theme::textDim()));
        p.drawText(r, Qt::AlignCenter, letter);
    });
    b->setToolTip(help);
    QObject::connect(b, &QAbstractButton::clicked, b, [action] { action(); });
    return b;
}

/// Track level: drag sideways to change, or click to type a value (Return or clicking away confirms, Esc cancels).
class GainPill : public QWidget {
public:
    GainPill(std::function<double()> db, std::function<void(double)> onChange, std::function<void()> onEnd, QWidget* parent = nullptr)
        : QWidget(parent), db_(std::move(db)), onChange_(std::move(onChange)), onEnd_(std::move(onEnd)) {
        setFixedSize(46, 22);
        setToolTip(QStringLiteral("Track level — drag, or click to type"));
        field_ = new QLineEdit(this);
        field_->setGeometry(rect());
        field_->setAlignment(Qt::AlignCenter);
        field_->setFont(Theme::mono(10, 500));
        field_->setFrame(false);
        field_->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; }")
                                  .arg(Theme::hover().name(QColor::HexArgb), Theme::text().name(), alpha(Theme::text(), 0.85).name(QColor::HexArgb)));
        field_->hide();
        field_->installEventFilter(this);
        connect(field_, &QLineEdit::returnPressed, this, [this] { commit(); });
    }
protected:
    void paintEvent(QPaintEvent*) override {
        if (editing_) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double db = db_();
        roundRect(p, QRectF(rect()), 6, underMouse() || startDB_ ? Theme::hover() : Theme::field());
        p.setFont(Theme::mono(10, 500));
        p.setPen(std::abs(db) > 0.05 ? Theme::text() : Theme::textDim());
        p.drawText(rect(), Qt::AlignCenter, formatDB(db).replace(QStringLiteral(" dB"), QString()));
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
    void mousePressEvent(QMouseEvent* e) override { press_ = e->position(); moved_ = false; }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || editing_) return;
        const double dx = e->position().x() - press_.x();
        if (!moved_ && std::abs(dx) < 3 && std::abs(e->position().y() - press_.y()) < 3) return;
        moved_ = true;
        if (!startDB_) startDB_ = db_();
        const double raw = *startDB_ + dx * 0.15;
        onChange_(std::min(12.0, std::max(-60.0, std::round(raw * 10) / 10)));
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        if (startDB_) {
            startDB_.reset();
            onEnd_();
            update();
            return;
        }
        if (moved_ || editing_) return;
        const double db = db_();
        field_->setText(db <= -59.5 ? QStringLiteral("-60") : QString::asprintf("%.1f", db));
        editing_ = true;
        field_->show();
        field_->setFocus();
        field_->selectAll();
        update();
    }
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == field_ && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) { stop(); return true; }
        if (o == field_ && e->type() == QEvent::FocusOut && editing_) commit();
        return QWidget::eventFilter(o, e);
    }
private:
    void commit() {
        QString cleaned;
        for (QChar ch : QString(field_->text()).replace(u',', u'.').replace(QChar(0x2212), u'-'))
            if (QStringLiteral("0123456789.-").contains(ch)) cleaned += ch;
        bool ok = false;
        const double v = cleaned.toDouble(&ok);
        stop();
        if (ok) {
            onChange_(std::min(12.0, std::max(-60.0, std::round(v * 10) / 10)));
            onEnd_();
        }
    }
    void stop() {
        editing_ = false;
        field_->hide();
        update();
    }
    std::function<double()> db_;
    std::function<void(double)> onChange_;
    std::function<void()> onEnd_;
    QLineEdit* field_;
    bool editing_ = false, moved_ = false;
    QPointF press_;
    std::optional<double> startDB_;
};

} // namespace

class TrackHeaders : public QWidget {
public:
    TrackHeaders(Controller& c, TimelineCanvas& canvas, QWidget* parent) : QWidget(parent), c_(c), canvas_(canvas) {
        setFixedWidth(int(TimelineLayout::headerWidth));
        // A drop over the headers is handed to the timeline, and lands at the start of that row.
        setAcceptDrops(true);
        corner_ = new QWidget(this);
        connect(&c_.model(), &EditorModel::changed, this, &TrackHeaders::rebuildIfNeeded);
        connect(&c_.model(), &EditorModel::selectionChanged, this, [this] { for (auto* w : findChildren<QWidget*>()) w->update(); });
        connect(&c_.model(), &EditorModel::viewChanged, this, &TrackHeaders::place);
        rebuildIfNeeded();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Theme::bg());
    }
    void resizeEvent(QResizeEvent*) override { place(); }
    // The track headers sit beside the timeline; scrolling over them scrolls the timeline too.
    void wheelEvent(QWheelEvent* e) override { canvas_.scrollBy(e); }
    // Drags over the headers: handed to the timeline, at the same height (left of its first frame).
    QPointF onCanvas(QPointF p) const { return QPointF(canvas_.mapFromGlobal(mapToGlobal(p.toPoint()))); }
    void dragEnterEvent(QDragEnterEvent* e) override {
        QDragEnterEvent f(onCanvas(e->position()).toPoint(), e->possibleActions(), e->mimeData(), e->buttons(), e->modifiers());
        QCoreApplication::sendEvent(&canvas_, &f);
        if (f.isAccepted()) e->acceptProposedAction(); else e->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        QDragMoveEvent f(onCanvas(e->position()).toPoint(), e->possibleActions(), e->mimeData(), e->buttons(), e->modifiers());
        QCoreApplication::sendEvent(&canvas_, &f);
        if (f.isAccepted()) e->acceptProposedAction(); else e->ignore();
    }
    void dragLeaveEvent(QDragLeaveEvent* e) override { QCoreApplication::sendEvent(&canvas_, e); }
    void dropEvent(QDropEvent* e) override {
        QDropEvent f(onCanvas(e->position()), e->possibleActions(), e->mimeData(), e->buttons(), e->modifiers());
        QCoreApplication::sendEvent(&canvas_, &f);
        if (f.isAccepted()) e->acceptProposedAction();
    }
private:
    struct Row { QWidget* widget; Uuid track; };

    QByteArray shape() const {
        const Project& p = c_.model().project();
        QByteArray k;
        for (const auto& t : p.videoTracks) k += t.id.toByteArray() + 'v';
        for (const auto& t : p.audioTracks) k += t.id.toByteArray() + 'a';
        k += p.captions ? 'c' : '-';
        return k;
    }

    void rebuildIfNeeded() {
        const QByteArray now = shape();
        if (now == shown_) { for (auto* w : findChildren<QWidget*>()) w->update(); place(); return; }
        shown_ = now;
        for (auto& r : rows_) r.widget->deleteLater();
        rows_.clear();
        if (caption_) { caption_->deleteLater(); caption_ = nullptr; }
        if (!addVideo_) {
            addVideo_ = addTrackButton(QStringLiteral("Video"), TrackKind::video);
            addAudio_ = addTrackButton(QStringLiteral("Audio"), TrackKind::audio);
        }
        const Project& p = c_.model().project();
        for (size_t i = 0; i < p.videoTracks.size(); ++i) rows_.push_back({trackHeader(p.videoTracks[i].id, QStringLiteral("V%1").arg(i + 1)), p.videoTracks[i].id});
        for (size_t i = 0; i < p.audioTracks.size(); ++i) rows_.push_back({trackHeader(p.audioTracks[i].id, QStringLiteral("A%1").arg(i + 1)), p.audioTracks[i].id});
        if (p.captions) caption_ = captionHeader();
        place();
    }

    void place() {
        const EditorModel& m = c_.model();
        const TimelineLayout layout(m.project(), m.view().rowHeight);
        const double ruler = TimelineLayout::ruler, sy = m.view().scrollY;
        for (const auto& row : layout.rows) {
            for (auto& r : rows_) {
                if (r.track != row.track.id) continue;
                r.widget->setGeometry(0, int(std::round(ruler + row.y - sy)), width(), int(row.height));
                r.widget->show();
            }
        }
        if (caption_ && layout.captionY) {
            caption_->setGeometry(0, int(std::round(ruler + *layout.captionY - sy)), width(), int(TimelineLayout::captionRow));
            caption_->show();
        }
        if (addVideo_) {
            addVideo_->setGeometry(0, int(std::round(ruler + layout.videoZoneY - sy)), width(), int(TimelineLayout::zone));
            addAudio_->setGeometry(0, int(std::round(ruler + layout.audioZoneY - sy)), width(), int(TimelineLayout::zone));
            addVideo_->show();
            addAudio_->show();
        }
        // Corner above the headers, level with the ruler.
        corner_->setGeometry(0, 0, width(), int(ruler));
        corner_->setStyleSheet(QStringLiteral("background: %1; border-bottom: 1px solid %2;").arg(Theme::panel().name(), Theme::stroke().name(QColor::HexArgb)));
        corner_->raise();
        corner_->show();
    }

    /// The track's own state, read when drawn (the headers aren't rebuilt for each change).
    const Track* track(const Uuid& id) const { return c_.model().project().track(id); }

    QWidget* addTrackButton(const QString& label, TrackKind kind) {
        auto* b = new Painted(QSize(int(TimelineLayout::headerWidth), int(TimelineLayout::zone)), [label](QPainter& p, const QRectF& r, bool hovering, bool) {
            if (hovering) p.fillRect(r, Theme::hover());
            const QColor c = hovering ? Theme::text() : Theme::textFaint();
            Symbols::draw(p, QStringLiteral("plus"), QPointF(12 + 4.5, r.center().y()), 9, c, 700);
            p.setFont(Theme::font(10, 500));
            p.setPen(c);
            p.drawText(QRectF(12 + 9 + 5, 0, r.width(), r.height()), Qt::AlignVCenter | Qt::AlignLeft, label);
        }, this);
        b->setMinimumWidth(0);
        b->setMaximumWidth(QWIDGETSIZE_MAX);
        b->setToolTip(QStringLiteral("Add %1 Track").arg(label));
        connect(b, &QAbstractButton::clicked, this, [this, kind] { c_.model().addTrack(kind); });
        return b;
    }

    /// A row's panel: the label, its keys, and for sound the level pill and meter.
    class RowPanel : public QWidget {
    public:
        RowPanel(std::function<void(QContextMenuEvent*)> menu, std::function<void()> click, QWidget* parent)
            : QWidget(parent), menu_(std::move(menu)), click_(std::move(click)) {}
    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.fillRect(rect(), Theme::panel());
            p.fillRect(QRectF(0, height() - 1, width(), 1), Theme::stroke());
        }
        void contextMenuEvent(QContextMenuEvent* e) override { if (menu_) menu_(e); }
        void mousePressEvent(QMouseEvent*) override { if (click_) click_(); }
    private:
        std::function<void(QContextMenuEvent*)> menu_;
        std::function<void()> click_;
    };

    QWidget* trackHeader(const Uuid& id, const QString& label) {
        EditorModel& m = c_.model();
        const Track* t0 = track(id);
        const TrackKind kind = t0 ? t0->kind : TrackKind::video;
        auto update = [&m, id](std::function<void(Track&)> body) { m.mutate([&](Project& p) { p.updateTrack(id, body); }); };
        auto* row = new RowPanel([this, id](QContextMenuEvent* e) { trackMenu(id, e->globalPos()); }, {}, this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(12, 0, 6, 0);
        h->setSpacing(3);
        // The label: dim while the track is off; a small accent arrow when it ducks under another.
        auto* name = new Painted(QSize(24, 20), [this, id, label](QPainter& p, const QRectF& r, bool, bool) {
            const Track* t = track(id);
            if (!t) return;
            const bool off = t->locked || (t->kind == TrackKind::video ? t->hidden : !c_.model().project().isAudible(*t));
            const QFont f = Theme::mono(11.5, 600);
            p.setFont(f);
            p.setPen(off ? Theme::textFaint() : Theme::text());
            p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, label);
            if (t->duckUnder) Symbols::draw(p, QStringLiteral("arrow.down"), QPointF(QFontMetricsF(f).horizontalAdvance(label) + 1 + 3.5, r.center().y()), 7, Theme::accent(), 800);
        }, row);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        if (t0 && t0->duckUnder) name->setToolTip(QStringLiteral("Ducks under another track"));
        h->addWidget(name);
        if (kind == TrackKind::audio) {
            h->addWidget(letterToggle(QStringLiteral("M"), [this, id] { const Track* t = track(id); return t && t->muted; }, QStringLiteral("Mute"),
                                      [update] { update([](Track& t) { t.muted = !t.muted; }); }));
            h->addWidget(letterToggle(QStringLiteral("S"), [this, id] { const Track* t = track(id); return t && t->solo; }, QStringLiteral("Solo"),
                                      [update] { update([](Track& t) { t.solo = !t.solo; }); }));
        } else {
            auto* eye = miniKey([this, id] { const Track* t = track(id); return t && t->hidden ? QStringLiteral("eye.slash") : QStringLiteral("eye"); },
                                [this, id] { const Track* t = track(id); return t && t->hidden; },
                                [this, id] { const Track* t = track(id); return t && t->hidden ? QStringLiteral("Show") : QStringLiteral("Hide"); },
                                [update] { update([](Track& t) { t.hidden = !t.hidden; }); });
            h->addWidget(eye);
        }
        h->addWidget(miniKey([] { return QStringLiteral("lock"); }, [this, id] { const Track* t = track(id); return t && t->locked; },
                             [this, id] { const Track* t = track(id); return t && t->locked ? QStringLiteral("Unlock") : QStringLiteral("Lock"); },
                             [update] { update([](Track& t) { t.locked = !t.locked; }); }));
        h->addStretch(1);
        if (kind == TrackKind::audio) {
            h->addWidget(new GainPill([this, id] { const Track* t = track(id); return t ? t->gainDB : 0.0; },
                                      [&m, id](double v) { m.live([&](Project& p) { p.updateTrack(id, [v](Track& t) { t.gainDB = v; }); }, false); },
                                      [&m] { m.commitLive(); }, row));
            auto* meter = new LevelMeter(c_, id, LevelMeter::Side::both, row);
            auto* mv = new QVBoxLayout;
            mv->setContentsMargins(3, 8, 0, 8);
            mv->addWidget(meter);
            h->addLayout(mv);
        }
        return row;
    }

    QWidget* captionHeader() {
        EditorModel& m = c_.model();
        auto* row = new RowPanel([this](QContextMenuEvent* e) {
            EditorModel& mm = c_.model();
            if (!mm.project().captions) return;
            const bool hidden = mm.project().captions->hidden;
            QMenu menu;
            menu.addAction(hidden ? QStringLiteral("Show Captions") : QStringLiteral("Hide Captions"), [&mm, hidden] { mm.setCaptionsHidden(!hidden); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete Caption Track"), [&mm] { mm.removeCaptionTrack(); });
            menu.exec(e->globalPos());
        }, [&m] { m.focusCaptions(); }, this);
        row->setToolTip(QStringLiteral("Captions: click for their style"));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(12, 0, 6, 0);
        h->setSpacing(3);
        auto hidden = [this] { return c_.model().project().captions && c_.model().project().captions->hidden; };
        auto* name = new Painted(QSize(24, 20), [hidden](QPainter& p, const QRectF& r, bool, bool) {
            p.setFont(Theme::mono(11.5, 600));
            p.setPen(hidden() ? Theme::textFaint() : Theme::text());
            p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("C1"));
        }, row);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        h->addWidget(name);
        h->addWidget(miniKey([hidden] { return hidden() ? QStringLiteral("eye.slash") : QStringLiteral("eye"); }, hidden,
                             [hidden] { return hidden() ? QStringLiteral("Show Captions") : QStringLiteral("Hide Captions"); },
                             [&m, hidden] { m.setCaptionsHidden(!hidden()); }));
        auto* label = new Painted(QSize(64, 20), [this](QPainter& p, const QRectF& r, bool, bool) {
            const EditorModel& mm = c_.model();
            const bool focused = mm.captionsFocused() || !mm.captionSelection().empty();
            p.setFont(Theme::font(10.5, 500));
            p.setPen(focused ? Theme::text() : Theme::textDim());
            p.drawText(r.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Captions"));
        }, row);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        h->addWidget(label);
        h->addStretch(1);
        return row;
    }

    void trackMenu(const Uuid& id, QPoint at) {
        EditorModel& m = c_.model();
        const Track* t = track(id);
        if (!t) return;
        const Track tr = *t;
        QMenu menu;
        if (tr.kind == TrackKind::audio) {
            QMenu* duck = menu.addMenu(QStringLiteral("Duck Under"));
            auto check = [](QAction* a, bool on) { a->setCheckable(true); a->setChecked(on); };
            check(duck->addAction(QStringLiteral("None"), [&m, id] { m.duck(id, std::nullopt); }), !tr.duckUnder);
            duck->addSeparator();
            const auto& audio = m.project().audioTracks;
            for (size_t i = 0; i < audio.size(); ++i) {
                if (audio[i].id == id) continue;
                const Uuid other = audio[i].id;
                check(duck->addAction(QStringLiteral("A%1").arg(i + 1), [&m, id, other] { m.duck(id, other); }), tr.duckUnder == other);
            }
            duck->addSeparator();
            for (double db : {-6.0, -12.0, -18.0, -24.0}) {
                const auto under = tr.duckUnder;
                check(duck->addAction(QStringLiteral("Down %1 dB").arg(int(-db)), [&m, id, under, db] { m.duck(id, under, db); }), std::abs(tr.duckDB - db) < 0.01);
            }
            menu.addSeparator();
        }
        menu.addAction(QStringLiteral("Add Video Track"), [&m] { m.addTrack(TrackKind::video); });
        menu.addAction(QStringLiteral("Add Audio Track"), [&m] { m.addTrack(TrackKind::audio); });
        if (m.project().tracks(tr.kind).size() > 1) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete Track"), [&m, id] { m.removeTrack(id); });
        }
        menu.exec(at);
    }

    Controller& c_;
    TimelineCanvas& canvas_;
    std::vector<Row> rows_;
    QWidget* caption_ = nullptr;
    QWidget* addVideo_ = nullptr;
    QWidget* addAudio_ = nullptr;
    QWidget* corner_;
    QByteArray shown_;
};

// MARK: Range bars

std::pair<double, double> gripDrag(int part, double a0, double b0, double move, double bar, double minLength, double maxLength, double mirror) {
    // Positive `grow` pulls the dragged grip outward.
    double grow = part == 2 ? move : -move;
    grow = std::min(grow, part == 2 ? bar - b0 : a0);
    auto edges = [&](double g) {
        return part == 2 ? std::make_pair(std::max(0.0, a0 - g * mirror), b0 + g) : std::make_pair(a0 - g, std::min(bar, b0 + g * mirror));
    };
    auto fits = [&](double g) {
        const auto e = edges(g);
        return e.second - e.first >= minLength - 0.0001 && e.second - e.first <= maxLength + 0.0001;
    };
    if (!fits(grow)) {
        // Length only ever increases with `grow`, so close in on the limit from the starting size.
        double ok = 0, bad = grow;
        for (int i = 0; i < 40; ++i) {
            const double mid = (ok + bad) / 2;
            if (fits(mid)) ok = mid; else bad = mid;
        }
        grow = ok;
    }
    return edges(grow);
}

RangeBar::RangeBar(Controller& c, bool vertical, QWidget* parent) : QWidget(parent), c_(c), vertical_(vertical) {
    if (vertical) setFixedWidth(int(RangeBarStyle::thickness)); else setFixedHeight(int(RangeBarStyle::thickness));
    setMouseTracking(true);
    connect(&c_.model(), &EditorModel::viewChanged, this, qOverload<>(&QWidget::update));
    connect(&c_.model(), &EditorModel::changed, this, qOverload<>(&QWidget::update));
}

double RangeBar::top() const { return vertical_ ? TimelineLayout::ruler : 0; }
double RangeBar::barLength() const { return std::max(1.0, vertical_ ? height() - top() : double(width())); }

std::pair<double, double> RangeBar::thumb() const {
    const EditorModel& m = c_.model();
    const TimelineViewState& v = m.view();
    const double minLen = RangeBarStyle::handle * 2 + 6;
    const double bar = barLength();
    if (!vertical_) {
        const double extent = m.extent();
        const double len = std::max(minLen, std::min(1.0, v.viewWidth / v.pps / extent) * bar);
        const double x = std::min(std::max(0.0, v.scrollX / v.pps / extent * bar), bar - len);
        return {x, len};
    }
    const double world = m.rowsWorld();
    const double len = std::max(minLen, m.rowWindow() * bar);
    const double y = std::min(std::max(0.0, (v.scrollY - m.minScrollY()) / world * bar), bar - len);
    return {y, len};
}

std::optional<int> RangeBar::part(double p) const {
    const auto [t, length] = thumb();
    if (p < t - 2 || p > t + length + 2) return std::nullopt;
    const double grip = std::min(RangeBarStyle::handle + 3, length / 3);
    if (p < t + grip) return 0;
    if (p > t + length - grip) return 2;
    return 1;
}

void RangeBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::panel());
    // The hairline along its inner edge.
    if (vertical_) p.fillRect(QRectF(0, 0, 1, height()), Theme::stroke()); else p.fillRect(QRectF(0, 0, width(), 1), Theme::stroke());
    const auto [t, len] = thumb();
    const double h = RangeBarStyle::handle;
    const QRectF r = vertical_ ? QRectF((width() - h) / 2, top() + t, h, len) : QRectF(t, (height() - h) / 2, len, h);
    const bool lit = hot_ || drag_;
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::ink(lit ? 0.3 : 0.2));
    p.drawRoundedRect(r, h / 2, h / 2);
    // The grips work as a pair, so they light up as a pair.
    const bool gripHot = hot_ == 0 || hot_ == 2;
    p.setBrush(Theme::bg());
    p.setPen(QPen(gripHot ? Theme::ink() : Theme::ink(0.6), 2));
    const double rr = h / 2 - 1;
    if (vertical_) {
        p.drawEllipse(QPointF(r.center().x(), r.top() + h / 2), rr, rr);
        p.drawEllipse(QPointF(r.center().x(), r.bottom() - h / 2), rr, rr);
    } else {
        p.drawEllipse(QPointF(r.left() + h / 2, r.center().y()), rr, rr);
        p.drawEllipse(QPointF(r.right() - h / 2, r.center().y()), rr, rr);
    }
}

void RangeBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    EditorModel& m = c_.model();
    const TimelineViewState& v = m.view();
    const double pos = vertical_ ? e->position().y() - top() : e->position().x();
    const double bar = barLength();
    BarDrag d;
    d.from = vertical_ ? e->position().y() : e->position().x();
    const auto [t, len] = thumb();
    if (auto pp = part(pos)) {
        d.part = *pp;
    } else if (!vertical_) {
        // Click on the empty bar: centre the thumb there, then keep dragging it.
        const double start = (pos - len / 2) / bar * m.extent();
        m.setView(std::nullopt, start * v.pps);
        d.part = 1;
    } else {
        m.setView(std::nullopt, std::nullopt, m.minScrollY() + (pos - len / 2) / bar * m.rowsWorld());
        d.part = 1;
    }
    d.pps = v.pps;
    d.scrollX = v.scrollX;
    d.scrollY = v.scrollY;
    d.rowHeight = v.rowHeight;
    if (!vertical_) {
        // Zoom around the playhead: the far edge moves just enough to hold the playhead where it is on screen.
        // With the playhead at the far edge that edge doesn't move at all; with it in the middle both edges move
        // equally. If the playhead is off screen, or nearer the grip being dragged, fall back to the middle.
        const double tt = (m.playhead() * v.pps - v.scrollX) / std::max(1.0, v.viewWidth);
        if (d.part == 2 && tt >= 0 && tt <= 0.5) d.mirror = tt / (1 - tt);
        else if (d.part == 0 && tt >= 0.5 && tt <= 1) d.mirror = (1 - tt) / tt;
        else d.mirror = 1;
    } else {
        d.length = m.rowWindow() * bar;
        d.content = m.rowsWorld();
        d.start = (v.scrollY - m.minScrollY()) / m.rowsWorld() * bar;
        d.anchor = m.rowAnchor(v.scrollY + m.rowsVisible() / 2);
    }
    drag_ = d;
    hot_ = d.part;
    update();
}

void RangeBar::mouseMoveEvent(QMouseEvent* e) {
    EditorModel& m = c_.model();
    const double pos = vertical_ ? e->position().y() : e->position().x();
    if (!drag_) {
        hot_ = part(vertical_ ? pos - top() : pos);
        update();
        return;
    }
    const BarDrag& d = *drag_;
    const double move = pos - d.from;
    const double bar = barLength();
    const TimelineViewState& v = m.view();
    if (!vertical_) {
        const double extent = m.extent();
        if (d.part == 1) {
            m.setView(std::nullopt, d.scrollX + move / bar * extent * d.pps);
        } else {
            // The grip stays under the mouse and the other end mirrors it.
            const double a0 = d.scrollX / d.pps / extent * bar;
            const auto ed = gripDrag(d.part, a0, a0 + v.viewWidth / d.pps / extent * bar, move, bar,
                                     v.viewWidth / TimelineViewState::maxPPS / extent * bar, bar, d.mirror);
            const double pps = v.viewWidth / ((ed.second - ed.first) / bar * extent);
            m.setView(pps, ed.first / bar * extent * pps);
        }
        return;
    }
    if (d.part == 1) {
        m.setView(std::nullopt, std::nullopt, d.scrollY + move / bar * d.content);
        return;
    }
    // A shorter thumb means taller rows. The grip stays under the mouse and the other end mirrors it.
    const auto ed = gripDrag(d.part, d.start, d.start + d.length, move, bar, m.rowWindow(TimelineViewState::rowMax) * bar,
                             m.rowWindow(TimelineViewState::rowMin) * bar);
    // Rows are whole points tall, so take the height whose thumb is closest to the one asked for.
    double best = v.rowHeight, miss = INFINITY;
    for (double r = TimelineViewState::rowMin; r <= TimelineViewState::rowMax; r += 1) {
        const double off = std::abs(m.rowWindow(r) * bar - (ed.second - ed.first));
        if (off < miss) { miss = off; best = r; }
    }
    // Whatever was in the middle of the timeline stays there, so rows grow and shrink around it.
    const double centre = d.anchor.first + d.anchor.second * best;
    m.setView(std::nullopt, std::nullopt, centre - m.rowsVisible() / 2, best);
}

void RangeBar::mouseReleaseEvent(QMouseEvent*) {
    drag_.reset();
    hot_.reset();
    update();
}

void RangeBar::leaveEvent(QEvent*) {
    if (!drag_) { hot_.reset(); update(); }
}

// MARK: Panel

TimelinePanel::TimelinePanel(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    toolbar_ = new TimelineToolbar(c_, this);
    v->addWidget(toolbar_);
    v->addWidget(new Hairline(Qt::Horizontal));
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    canvas_ = new TimelineCanvas(c_);
    headers_ = new TrackHeaders(c_, *canvas_, this);
    body->addWidget(headers_);
    body->addWidget(new Hairline(Qt::Vertical));
    auto* right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(0);
    auto* top = new QHBoxLayout;
    top->setSpacing(0);
    top->addWidget(canvas_, 1);
    top->addWidget(new RangeBar(c_, true));
    right->addLayout(top, 1);
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(0);
    bottom->addWidget(new RangeBar(c_, false), 1);
    auto* corner = new QWidget;
    corner->setFixedSize(int(RangeBarStyle::thickness), int(RangeBarStyle::thickness));
    corner->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::panel().name()));
    bottom->addWidget(corner);
    right->addLayout(bottom);
    body->addLayout(right, 1);
    v->addLayout(body, 1);
}

void TimelinePanel::refreshToggles() { toolbar_->refresh(); }


} // namespace er
