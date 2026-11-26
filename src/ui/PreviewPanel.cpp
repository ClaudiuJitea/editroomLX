#include "PreviewPanel.h"
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include "LevelMeter.h"
#include "Symbols.h"
#include "Theme.h"
#include "Widgets.h"
#include "model/Timecode.h"
#include <QAbstractButton>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace er {

namespace {

QColor alpha(QColor c, double a) { c.setAlphaF(float(c.alphaF() * a)); return c; }

/// A button drawn by a function.
class Painted : public QAbstractButton {
public:
    using Paint = std::function<void(QPainter&, const QRectF&, bool hovering)>;
    Painted(QSize size, Paint paint, QWidget* parent = nullptr) : QAbstractButton(parent), paint_(std::move(paint)) { setFixedSize(size); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paint_(p, QRectF(rect()), underMouse());
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
    Paint paint_;
};

/// IconButton (Theme.swift): a symbol at 0.46 of the button's size; lit when active or hovered.
Painted* iconButton(std::function<QString()> symbol, int size, std::function<bool()> active, std::function<QString()> help, std::function<void()> action) {
    auto* b = new Painted(QSize(size, size), [symbol, size, active](QPainter& p, const QRectF& r, bool hovering) {
        const bool on = active && active();
        p.setPen(Qt::NoPen);
        if (on) { p.setBrush(Theme::ink(0.1)); p.drawRoundedRect(r, 7, 7); }
        else if (hovering) { p.setBrush(Theme::hover()); p.drawRoundedRect(r, 7, 7); }
        Symbols::draw(p, symbol(), r.center(), size * 0.46, on || hovering ? Theme::text() : Theme::textDim(), 500);
    });
    b->setToolTip(help());
    QObject::connect(b, &QAbstractButton::clicked, b, [b, action, help] { action(); b->setToolTip(help()); b->update(); });
    return b;
}

} // namespace

// MARK: Viewer

namespace {
/// The window frames are presented in: input passes through to the viewer under it (dragging the picture).
class PresentWindow : public QWindow {
public:
    PresentWindow(PlaybackEngine& engine, QVulkanInstance* vk, std::function<void()> ready) : engine_(engine), ready_(std::move(ready)) {
        setSurfaceType(QSurface::VulkanSurface);
        setVulkanInstance(vk);
        setFlag(Qt::WindowTransparentForInput);
    }
    ~PresentWindow() override { release(); }
    bool ready() const { return surface_ != nullptr; }
protected:
    /// The swapchain goes before the surface does (the window container destroys the platform window, surface and
    /// all, before this window's destructor runs).
    bool event(QEvent* e) override {
        if (e->type() == QEvent::PlatformSurface
            && static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            release();
        return QWindow::event(e);
    }
    void release() {
        if (surface_) engine_.setPresentSurface(nullptr, 0, 0);
        surface_ = nullptr;
    }
    void exposeEvent(QExposeEvent*) override {
        if (!isExposed() || surface_) return;
        surface_ = reinterpret_cast<void*>(QVulkanInstance::surfaceForWindow(this));
        if (surface_ && !engine_.setPresentSurface(surface_, pixels().width(), pixels().height())) surface_ = nullptr;
        if (ready_) ready_();
    }
    void resizeEvent(QResizeEvent*) override {
        if (!surface_) return;
        engine_.setPresentSurface(surface_, pixels().width(), pixels().height());
        engine_.redraw();
    }
private:
    QSize pixels() const { return (QSizeF(size()) * devicePixelRatio()).toSize(); }
    PlaybackEngine& engine_;
    std::function<void()> ready_;
    void* surface_ = nullptr;
};
} // namespace

bool Viewer::presenting() const { return present_ && presentBox_ && presentBox_->isVisible() && c_.engine().presenting(); }

void Viewer::updatePresenting() {
    // On X11 and Wayland, whenever there's a window system to present to (EDITROOM_PRESENT=0 always paints).
    static const bool allowed = qEnvironmentVariable("EDITROOM_PRESENT") != QLatin1String("0")
        && (QGuiApplication::platformName() == QLatin1String("xcb") || QGuiApplication::platformName() == QLatin1String("wayland"));
    auto* panel = qobject_cast<PreviewPanel*>(parentWidget() ? parentWidget()->parentWidget() ? parentWidget()->parentWidget()->parentWidget() : nullptr : nullptr);
    const bool zoomed = panel && panel->zoom();
    // (Not while Magic's thinking scene is over the window: it dims and draws on the picture.)
    const bool overlay = window() && window()->property("magicOverlay").toBool();
    const bool wanted = allowed && isVisible() && !zoomed && !overlay && !c_.model().project().clips.empty() && !c_.sourcePreview().current();
    if (wanted && !present_ && c_.engine().presentInstance()) {
        vulkan_ = std::make_unique<QVulkanInstance>();
        vulkan_->setVkInstance(reinterpret_cast<VkInstance>(c_.engine().presentInstance()));
        if (vulkan_->create()) {
            auto* w = new PresentWindow(c_.engine(), vulkan_.get(), [this] { updatePresenting(); });
            present_ = w;
            presentBox_ = QWidget::createWindowContainer(w, this);
            // Only this window is native: not the viewer, the panels and the window around it.
            presentBox_->setAttribute(Qt::WA_DontCreateNativeAncestors);
            presentBox_->setAttribute(Qt::WA_TransparentForMouseEvents);
            presentBox_->setFocusPolicy(Qt::NoFocus);
            presentBox_->setGeometry(picture().toAlignedRect());
        } else {
            vulkan_.reset();
        }
    }
    if (!present_) return;
    const QRect place = picture().toAlignedRect();
    const bool ready = static_cast<PresentWindow*>(present_)->ready();
    presentBox_->setGeometry(place);
    presentBox_->setVisible(wanted);
    c_.engine().setPresenting(wanted && ready);
}

Viewer::Viewer(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
    setMouseTracking(true);
    connect(&c_, &Controller::frame, this, [this](const QImage& img, double) { setFrame(img); });
    connect(&c_.sourcePreview(), &SourcePreview::changed, this, [this] { updatePresenting(); update(); });
    connect(&c_.model(), &EditorModel::changed, this, [this] { updatePresenting(); update(); });
    connect(&c_.model(), &EditorModel::viewerOnlyChanged, this, [this] { updatePresenting(); update(); });
}

QRectF Viewer::picture() const {
    const Project& proj = c_.model().project();
    const double aspect = double(proj.width) / std::max(1, proj.height);
    auto* panel = qobject_cast<PreviewPanel*>(parentWidget() ? parentWidget()->parentWidget() ? parentWidget()->parentWidget()->parentWidget() : nullptr : nullptr);
    double w, h;
    if (panel && panel->zoom()) {
        // 100% is one frame pixel per screen pixel.
        w = proj.width * *panel->zoom() / devicePixelRatioF();
        h = w / aspect;
    } else {
        w = std::min(double(width()), height() * aspect);
        h = w / aspect;
    }
    return QRectF((width() - w) / 2, (height() - h) / 2, w, h);
}

std::optional<Clip> Viewer::target() const {
    const EditorModel& m = c_.model();
    std::vector<Clip> visual;
    for (const auto& id : m.selection()) if (const Clip* cl = m.project().clip(id); cl && cl->kind != ClipKind::audio) visual.push_back(*cl);
    return visual.size() == 1 ? std::optional<Clip>(visual.front()) : std::nullopt;
}

QRectF Viewer::promptRect() const {
    const QRectF r = picture();
    return QRectF(r.center().x() - r.width() * 0.31, r.center().y() - r.height() * 0.29, r.width() * 0.62, r.height() * 0.58);
}

/// What the viewer shows on an empty timeline: a dashed drop area with a film frame and an arrow. The viewer is
/// black in every theme, so on Light this uses Dark's colours.
void Viewer::drawEmptyPrompt(QPainter& p, const QRectF& area) {
    const Palette& pal = Theme::isLight() ? Palette::dark : Theme::p();
    const bool hasMedia = !c_.model().project().assets.empty();
    const QColor text = Theme::hex(pal.text), faint = Theme::hex(pal.textFaint);
    const QRectF box = promptRect();
    QPen dash(promptHover_ ? Theme::hex(pal.ink, 0.3) : Theme::hex(pal.outline), 1.5);
    dash.setDashPattern({5 / 1.5, 4 / 1.5});
    p.setPen(dash);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(box.adjusted(0.75, 0.75, -0.75, -0.75), 16, 16);
    // The film frame and the arrow, on a 74-wide grid, 96 × 70.
    const QFont title = Theme::font(13, 600), sub = Theme::font(11.5);
    const double total = 70 + 16 + QFontMetricsF(title).height() + 16 + QFontMetricsF(sub).height();
    const QRectF art(box.center().x() - 48, box.center().y() - total / 2, 96, 70);
    const double s = art.width() / 74;
    auto pt = [&](double x, double y) { return QPointF(art.left() + x * s, art.top() + y * s); };
    QPainterPath frame;
    frame.addRoundedRect(QRectF(pt(1, 9), pt(59, 49)), 5 * s, 5 * s);
    for (double y : {17.0, 41.0}) { frame.moveTo(pt(1, y)); frame.lineTo(pt(59, y)); }
    for (double x : {9.0, 19.0, 29.0, 39.0, 49.0}) {
        frame.moveTo(pt(x, 9)); frame.lineTo(pt(x, 17));
        frame.moveTo(pt(x, 41)); frame.lineTo(pt(x, 49));
    }
    p.setPen(QPen(faint, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(frame);
    QPainterPath arrow;
    arrow.moveTo(pt(66, 4)); arrow.lineTo(pt(66, 24));
    arrow.moveTo(pt(59, 17)); arrow.lineTo(pt(66, 24)); arrow.lineTo(pt(73, 17));
    p.setPen(QPen(text, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(arrow);
    double y = art.bottom() + 16;
    p.setFont(title);
    p.setPen(text);
    p.drawText(QRectF(area.left(), y, area.width(), QFontMetricsF(title).height()), Qt::AlignHCenter | Qt::AlignTop,
               hasMedia ? QStringLiteral("Drag media from the library to the timeline") : QStringLiteral("Drop video, audio or images anywhere"));
    y += QFontMetricsF(title).height() + 16;
    p.setFont(sub);
    p.setPen(faint);
    p.drawText(QRectF(area.left(), y, area.width(), QFontMetricsF(sub).height()), Qt::AlignHCenter | Qt::AlignTop,
               hasMedia ? QStringLiteral("or double-click it to add it at the playhead")
                        : QStringLiteral("or press Ctrl+I to import  ·  MP4, MOV, MKV, WebM, WAV, MP3, JPG, PNG"));
}

void Viewer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    const EditorModel& m = c_.model();
    const Project& proj = m.project();
    p.fillRect(rect(), m.viewerOnly() ? QColor(Qt::black) : Theme::bg());
    const QRectF r = picture();
    QPainterPath clip;
    clip.addRoundedRect(r, 3, 3);
    p.save();
    p.setClipPath(clip);
    p.fillRect(r, Qt::black);
    if (!frame_.isNull() && !proj.clips.empty()) p.drawImage(r, frame_);
    // A library item being hover-scrubbed, or a trimmed edge's frame, shows here, labelled so it isn't mistaken
    // for the edit. Until its first frame is decoded, the viewer's own picture stays showing underneath.
    const auto& s = c_.sourcePreview().current();
    if (s) {
        if (!s->image.isNull()) {
            p.fillRect(r, Qt::black);
            const QSizeF fit = QSizeF(s->image.size()).scaled(r.size(), Qt::KeepAspectRatio);
            p.drawImage(QRectF(r.center().x() - fit.width() / 2, r.center().y() - fit.height() / 2, fit.width(), fit.height()), s->image);
        }
        const double fps = s->asset.fps > 1 ? s->asset.fps : 30;
        const QString time = Timecode::string(s->time, fps);
        const QFont name = Theme::font(11.5, 600), mono = Theme::mono(11.5, 400);
        const double nw = s->note.isEmpty() ? QFontMetricsF(name).horizontalAdvance(s->asset.name) + 6 : 0;
        const double tw = QFontMetricsF(mono).horizontalAdvance(time);
        const QRectF label(r.left() + 10, r.top() + 10, nw + tw + 16, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 166));
        p.drawRoundedRect(label, 6, 6);
        // On the picture, so light type in every theme. Trimming a clip's edge: just the time.
        if (s->note.isEmpty()) {
            p.setFont(name);
            p.setPen(QColor(0xF2, 0xF2, 0xF0));
            p.drawText(QRectF(label.left() + 8, label.top(), nw, label.height()), Qt::AlignVCenter | Qt::AlignLeft, s->asset.name);
        }
        p.setFont(mono);
        p.setPen(s->note.isEmpty() ? QColor(0x9A, 0x9A, 0x96) : QColor(0xF2, 0xF2, 0xF0));
        p.drawText(QRectF(label.left() + 8 + nw, label.top(), tw, label.height()), Qt::AlignVCenter | Qt::AlignLeft, time);
    }
    p.restore();
    p.setPen(QPen(Theme::ink(0.06), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    // Nothing on the timeline yet: say how to start, and take a click as "import".
    if (proj.clips.empty() && !s) drawEmptyPrompt(p, r);
}

void Viewer::mousePressEvent(QMouseEvent* e) {
    press_ = e->position();
    dragging_ = false;
}

void Viewer::mouseMoveEvent(QMouseEvent* e) {
    EditorModel& m = c_.model();
    if (!(e->buttons() & Qt::LeftButton)) {
        const bool h = m.project().clips.empty() && promptRect().contains(e->position());
        if (h != promptHover_) { promptHover_ = h; update(); }
        return;
    }
    if (!dragging_ && (e->position() - press_).manhattanLength() < 2) return;
    dragging_ = true;
    auto c = target();
    if (!c) return;
    const QRectF pic = picture();
    const double w = pic.width(), h = pic.height();
    const QPointF tr = e->position() - press_;
    if (c->kind == ClipKind::text) {
        if (!moveStart_) moveStart_ = std::make_pair(c->text.x, c->text.y);
        const double nx = std::clamp(moveStart_->first + tr.x() / w, 0.0, 1.0);
        const double ny = std::clamp(moveStart_->second + tr.y() / h, 0.0, 1.0);
        const Uuid id = c->id;
        m.live([&](Project& p) { if (auto i = p.index(id)) { p.clips[*i].text.x = nx; p.clips[*i].text.y = ny; } }, false);
    } else if (c->isTransformable()) {
        const ClipTransform tf = c->transform.value_or(ClipTransform());
        // A zooming clip: framed on the picture all the way in.
        if (!moveStart_ && tf.zoom) m.showZoomed(c->id);
        if (!moveStart_) moveStart_ = tf.zoomedOffset();
        double nx = moveStart_->first + tr.x() / w;
        double ny = moveStart_->second + tr.y() / h;
        // Snap back to centre within a few points.
        if (std::abs(nx) * w < 5) nx = 0;
        if (std::abs(ny) * h < 5) ny = 0;
        // Zoomed in, the picture can't be pulled past the frame's edge.
        if (tf.fills()) {
            const double mm = (tf.scale - 1) / 2;
            nx = std::clamp(nx, -mm, mm);
            ny = std::clamp(ny, -mm, mm);
        }
        const double width = m.project().width, height = m.project().height;
        const Uuid id = c->id;
        m.live([&](Project& p) {
            auto i = p.index(id);
            if (!i) return;
            ClipTransform t = p.clips[*i].transform.value_or(ClipTransform());
            t.x = std::round(nx * width) / width;
            t.y = std::round(ny * height) / height;
            if (t.isIdentity()) p.clips[*i].transform.reset(); else p.clips[*i].transform = t;
        }, false);
    }
}

void Viewer::mouseReleaseEvent(QMouseEvent* e) {
    EditorModel& m = c_.model();
    if (dragging_) {
        moveStart_.reset();
        m.commitLive();
    } else if (m.project().clips.empty() && m.project().assets.empty() && promptRect().contains(e->position())) {
        c_.showImportPanel();
    }
    dragging_ = false;
}

// MARK: Transport

/// The current time. Click it to type a time to jump to: Return or clicking away goes there, Esc cancels.
class TimecodeField : public QWidget {
public:
    explicit TimecodeField(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
        setFixedHeight(26);
        setToolTip(QStringLiteral("Click to type a time"));
        field_ = new QLineEdit(this);
        field_->setFrame(false);
        field_->setFont(font());
        field_->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 0 6px; }")
                                  .arg(Theme::field().name(), Theme::text().name(), alpha(Theme::text(), 0.85).name(QColor::HexArgb)));
        field_->hide();
        field_->installEventFilter(this);
        connect(field_, &QLineEdit::returnPressed, this, [this] { commit(); });
        refresh();
    }
    void refresh() {
        const EditorModel& m = c_.model();
        const double fps = m.project().fps;
        // The frame shown: the skimmer's while skimming, the playhead's otherwise.
        const double t = std::round(m.skimTime().value_or(m.playhead()) * fps) / fps;
        text_ = Timecode::string(t, fps);
        const QFont f = font();
        setFixedWidth(int(std::ceil(QFontMetricsF(f).horizontalAdvance(text_) + text_.size() * 0.4 + 14)));
        field_->setGeometry(rect());
        update();
    }
    QFont font() const {
        QFont f = Theme::mono(20, 500);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
        return f;
    }
    QString text() const { return text_; }
protected:
    void paintEvent(QPaintEvent*) override {
        if (editing_) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (underMouse()) { p.setPen(Qt::NoPen); p.setBrush(Theme::field()); p.drawRoundedRect(QRectF(rect()), 6, 6); }
        p.setFont(font());
        p.setPen(Theme::text());
        p.drawText(rect().adjusted(7, 0, -7, 0), Qt::AlignVCenter | Qt::AlignLeft, text_);
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
    void mousePressEvent(QMouseEvent*) override {
        if (editing_) return;
        editing_ = true;
        field_->setText(text_);
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
        const auto t = Timecode::parse(field_->text(), c_.model().project().fps);
        stop();
        if (t) c_.model().seek(*t);
    }
    void stop() {
        editing_ = false;
        field_->hide();
        update();
    }
    Controller& c_;
    QLineEdit* field_;
    QString text_;
    bool editing_ = false;
};

/// The play button: a 46 px disc in the text colour, set off from the bar by a dark ring.
class PlayButton : public QAbstractButton {
public:
    PlayButton(Controller& c, QWidget* parent) : QAbstractButton(parent), c_(c) {
        setFixedSize(56, 56);
        setToolTip(QStringLiteral("Play / Pause (Space)"));
        connect(this, &QAbstractButton::clicked, this, [this] { c_.model().togglePlay(); });
        connect(&c_.model(), &EditorModel::playingChanged, this, qOverload<>(&QWidget::update));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hovering = underMouse();
        const QPointF c(width() / 2.0, height() / 2.0);
        const double s = hovering ? 1.05 : 1;
        p.translate(c);
        p.scale(s, s);
        p.setPen(QPen(Theme::ink(0.05), 1));
        p.setBrush(Theme::transport());
        p.drawEllipse(QPointF(0, 0), 28 - 0.5, 28 - 0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(hovering ? Theme::ink() : Theme::text());
        p.drawEllipse(QPointF(0, 0), 23, 23);
        const bool playing = c_.model().isPlaying();
        Symbols::draw(p, playing ? QStringLiteral("pause.fill") : QStringLiteral("play.fill"), QPointF(playing ? 0 : 1.5, 0), 17, Theme::onText(), 700);
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    Controller& c_;
};

/// The viewer zoom: Fit, 25%, 50%, 100%, 200%.
class ZoomMenu : public QAbstractButton {
public:
    ZoomMenu(PreviewPanel* panel, QWidget* parent) : QAbstractButton(parent), panel_(panel) {
        setFixedHeight(24);
        setToolTip(QStringLiteral("Viewer zoom"));
        refresh();
        connect(this, &QAbstractButton::clicked, this, [this] {
            QMenu menu(this);
            for (const auto& [label, z] : zooms()) menu.addAction(label, this, [this, z] { panel_->setZoom(z); refresh(); });
            menu.exec(mapToGlobal(QPoint(0, height() + 2)));
        });
    }
    static std::vector<std::pair<QString, std::optional<double>>> zooms() {
        return {{QStringLiteral("Fit"), std::nullopt}, {QStringLiteral("25%"), 0.25}, {QStringLiteral("50%"), 0.5}, {QStringLiteral("100%"), 1.0}, {QStringLiteral("200%"), 2.0}};
    }
    QString label() const {
        for (const auto& [l, z] : zooms()) if (z == panel_->zoom()) return l;
        return QStringLiteral("Fit");
    }
    void refresh() {
        setFixedWidth(int(std::ceil(QFontMetricsF(Theme::font(11.5, 500)).horizontalAdvance(label()) + 4 + 8 + 16)));
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::field());
        p.drawRoundedRect(QRectF(rect()), 6, 6);
        p.setFont(Theme::font(11.5, 500));
        p.setPen(Theme::textDim());
        p.drawText(QRectF(8, 0, width() - 16, height()), Qt::AlignVCenter | Qt::AlignLeft, label());
        Symbols::draw(p, QStringLiteral("chevron.down"), QPointF(width() - 8 - 4, height() / 2.0), 7.5, Theme::textDim(), 700);
    }
private:
    PreviewPanel* panel_;
};

class TransportBar : public QWidget {
public:
    TransportBar(Controller& c, PreviewPanel* panel, QWidget* parent) : QWidget(parent), c_(c) {
        setFixedHeight(76);
        EditorModel& m = c_.model();
        // Left and right: the times; loop, meters, zoom, full screen.
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(16, 0, 16, 0);
        row->setSpacing(10);
        time_ = new TimecodeField(c_, this);
        row->addWidget(time_);
        // The length, on the time's baseline.
        length_ = new QLabel(this);
        length_->setFont(Theme::mono(11.5, 400));
        length_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textFaint().name()));
        length_->setFixedHeight(26);
        length_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        {
            const QFontMetricsF big(time_->font()), small(length_->font());
            const double baseline = 13 + (big.ascent() - big.descent()) / 2;
            length_->setContentsMargins(0, int(std::round(baseline - small.ascent())), 0, 0);
        }
        row->addWidget(length_);
        row->addStretch(1);
        row->addWidget(iconButton([] { return QStringLiteral("repeat"); }, 28, [&m] { return m.loop(); },
                                  [] { return QStringLiteral("Loop Playback (L)"); }, [&m] { m.setLoop(!m.loop()); }));
        // Output level, left and right.
        auto* meters = new QWidget(this);
        meters->setToolTip(QStringLiteral("Output level, left and right"));
        meters->setFixedSize(4 + 2 + 4 + 8, 24);
        auto* mh = new QHBoxLayout(meters);
        mh->setContentsMargins(4, 0, 4, 0);
        mh->setSpacing(2);
        mh->addWidget(new LevelMeter(c_, std::nullopt, LevelMeter::Side::left, meters));
        mh->addWidget(new LevelMeter(c_, std::nullopt, LevelMeter::Side::right, meters));
        row->addWidget(meters);
        zoom_ = new ZoomMenu(panel, this);
        row->addWidget(zoom_);
        row->addWidget(iconButton([&m] { return m.viewerOnly() ? QStringLiteral("arrow.down.right.and.arrow.up.left") : QStringLiteral("arrow.up.left.and.arrow.down.right"); },
                                  28, {}, [&m] { return m.viewerOnly() ? QStringLiteral("Exit Full Screen (F or Esc)") : QStringLiteral("Full Screen Viewer (F)"); },
                                  [&m] { m.toggleViewerFullScreen(); }));
        // In the middle of the bar: the transport.
        centre_ = new QWidget(this);
        auto* ch = new QHBoxLayout(centre_);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->setSpacing(4);
        ch->addWidget(iconButton([] { return QStringLiteral("backward.end.fill"); }, 32, {}, [] { return QStringLiteral("Previous Edit (Up or Shift+Left)"); },
                                 [&m] { m.jumpToEdit(false); }));
        ch->addWidget(iconButton([] { return QStringLiteral("backward.frame.fill"); }, 32, {}, [] { return QStringLiteral("Back One Frame (Left)"); },
                                 [&m] { m.step(-1); }));
        ch->addSpacing(10 - 4);
        ch->addWidget(new PlayButton(c_, centre_));
        ch->addSpacing(10 - 4);
        ch->addWidget(iconButton([] { return QStringLiteral("forward.frame.fill"); }, 32, {}, [] { return QStringLiteral("Forward One Frame (Right)"); },
                                 [&m] { m.step(1); }));
        ch->addWidget(iconButton([] { return QStringLiteral("forward.end.fill"); }, 32, {}, [] { return QStringLiteral("Next Edit (Down or Shift+Right)"); },
                                 [&m] { m.jumpToEdit(true); }));
        centre_->adjustSize();
        auto refresh = [this] {
            time_->refresh();
            length_->setText(QStringLiteral("/ ") + Timecode::string(c_.model().project().duration(), c_.model().project().fps));
            update();
            for (auto* w : findChildren<QWidget*>()) w->update();
        };
        connect(&m, &EditorModel::playheadChanged, this, refresh);
        connect(&m, &EditorModel::playingChanged, this, refresh);
        connect(&m, &EditorModel::changed, this, refresh);
        connect(&m, &EditorModel::viewerOnlyChanged, this, refresh);
        refresh();
    }
protected:
    void resizeEvent(QResizeEvent*) override { centre_->move((width() - centre_->width()) / 2, (height() - centre_->height()) / 2); }
private:
    Controller& c_;
    TimecodeField* time_;
    QLabel* length_;
    ZoomMenu* zoom_;
    QWidget* centre_;
};

// MARK: Panel

PreviewPanel::PreviewPanel(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* pad = new QVBoxLayout;
    pad->setContentsMargins(16, 16, 16, 0);
    area_ = new QScrollArea;
    area_->setFrameShape(QFrame::NoFrame);
    area_->setWidgetResizable(false);
    area_->setAlignment(Qt::AlignCenter);
    area_->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    viewer_ = new Viewer(c_);
    area_->setWidget(viewer_);
    pad->addWidget(area_, 1);
    v->addLayout(pad, 1);
    // The bar takes the whole width under the picture, so its controls sit in the middle of it.
    bar_ = new TransportBar(c_, this, this);
    v->addWidget(bar_);
    connect(&c_.model(), &EditorModel::changed, this, &PreviewPanel::place);
    connect(&c_.model(), &EditorModel::viewerOnlyChanged, this, [this] { update(); });
}

void PreviewPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), c_.model().viewerOnly() ? QColor(Qt::black) : Theme::bg());
}

void PreviewPanel::setZoom(std::optional<double> z) {
    zoom_ = z;
    place();
}

void PreviewPanel::resizeEvent(QResizeEvent*) {
    place();
    // The scroll area learns its new size after this: placed again once it has.
    QTimer::singleShot(0, this, &PreviewPanel::place);
}

/// Fitted, the viewer takes the space; zoomed bigger than the space, it scrolls.
void PreviewPanel::place() {
    const QSize space = area_->viewport()->size();
    QSize size = space;
    if (zoom_) {
        const Project& p = c_.model().project();
        const double w = p.width * *zoom_ / devicePixelRatioF(), h = w * p.height / std::max(1, p.width);
        size = QSize(std::max(space.width(), int(std::ceil(w))), std::max(space.height(), int(std::ceil(h))));
    }
    area_->setHorizontalScrollBarPolicy(size.width() > space.width() ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    area_->setVerticalScrollBarPolicy(size.height() > space.height() ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    viewer_->resize(size);
    viewer_->updatePresenting();
    viewer_->update();
}

} // namespace er
