#include "SettingsDialog.h"
#include "Updates.h"
#include "Alert.h"
#include "LibraryPanel.h"
#include "Symbols.h"
#include "Theme.h"
#include "Widgets.h"
#include "editor/EditorModel.h"
#include "model/Timecode.h"
#include <QAbstractButton>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QLocale>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace er {

std::vector<AgentApp>& SettingsDialog::agentApps() { static std::vector<AgentApp> a; return a; }
AgentServerHooks& SettingsDialog::server() { static AgentServerHooks h; return h; }

namespace {

QColor alpha(QColor c, double a) { c.setAlphaF(float(c.alphaF() * a)); return c; }

class Painted : public QAbstractButton {
public:
    using Paint = std::function<void(QPainter&, const QRectF&, bool hovering)>;
    Painted(QSize size, Paint paint, QWidget* parent = nullptr) : QAbstractButton(parent), paint_(std::move(paint)) { setFixedSize(size); }
    void setPaint(Paint p) { paint_ = std::move(p); update(); }
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

QLabel* text(const QString& s, double px, const QColor& color, int weight = 400, bool wrap = false) {
    auto* l = new QLabel(s);
    l->setFont(Theme::font(px, weight));
    l->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(color.name(QColor::HexArgb)));
    l->setWordWrap(wrap);
    return l;
}

/// SectionLabel: small caps over a group.
QLabel* smallCaps(const QString& s, double kerning = 1.4) {
    auto* l = new QLabel(s.toUpper());
    QFont f = Theme::label();
    f.setLetterSpacing(QFont::AbsoluteSpacing, kerning);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textFaint().name()));
    return l;
}

/// A rounded box in the field colour (optionally with the stroke), holding rows.
QWidget* box(double radius, bool stroke, QVBoxLayout** inner) {
    auto* w = new QWidget;
    w->setObjectName(QStringLiteral("settingsBox"));
    w->setAttribute(Qt::WA_StyledBackground);
    w->setStyleSheet(QStringLiteral("#settingsBox { background: %1; border-radius: %2px; border: 1px solid %3; }")
                         .arg(Theme::field().name()).arg(radius).arg(stroke ? Theme::stroke().name(QColor::HexArgb) : QStringLiteral("transparent")));
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    *inner = v;
    return w;
}

QWidget* divider(int indent = 0) {
    auto* d = new QWidget;
    d->setFixedHeight(1);
    auto* h = new QHBoxLayout(d);
    h->setContentsMargins(indent, 0, 0, 0);
    auto* line = new QWidget;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::stroke().name(QColor::HexArgb)));
    h->addWidget(line);
    return d;
}

/// SettingsSection: a small grey label over a rounded box of settings.
QWidget* settingsSection(const QString& title, QVBoxLayout** inner) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(7);
    auto* l = smallCaps(title, 1.5);
    l->setContentsMargins(2, 0, 0, 0);
    v->addWidget(l);
    v->addWidget(box(10, true, inner));
    return w;
}

/// SettingsRow: its name (and a line on what it does) on the left, its control on the right.
QWidget* settingsRow(const QString& title, QLabel** detail, QWidget* control) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(14, 10, 14, 10);
    h->setSpacing(14);
    auto* left = new QVBoxLayout;
    left->setSpacing(2);
    left->addWidget(text(title, 13, Theme::text()));
    if (detail) {
        *detail = text(QString(), 11, Theme::textDim());
        left->addWidget(*detail);
    }
    h->addLayout(left, 1);
    h->addWidget(control);
    return row;
}

/// SegmentedChoice: a segmented switch in Editroom's own greys.
QWidget* segmented(const QStringList& options, std::function<int()> current, std::function<void(int)> pick, bool mono = false) {
    auto* w = new QWidget;
    w->setObjectName(QStringLiteral("segmented"));
    w->setAttribute(Qt::WA_StyledBackground);
    w->setStyleSheet(QStringLiteral("#segmented { background: %1; border-radius: 7px; }").arg(Theme::ink(0.06).name(QColor::HexArgb)));
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(2, 2, 2, 2);
    h->setSpacing(0);
    const QFont f = mono ? Theme::mono(11.5, 400) : Theme::font(12);
    std::vector<Painted*> buttons;
    for (int i = 0; i < options.size(); ++i) {
        const QString name = options[i];
        auto* b = new Painted(QSize(int(std::ceil(QFontMetricsF(f).horizontalAdvance(name))) + 22, 22), {}, w);
        b->setPaint([name, f, current, i](QPainter& p, const QRectF& r, bool) {
            const bool on = current() == i;
            if (on) { p.setPen(Qt::NoPen); p.setBrush(Theme::ink(0.16)); p.drawRoundedRect(r, 5, 5); }
            p.setFont(f);
            p.setPen(on ? Theme::text() : Theme::textDim());
            p.drawText(r, Qt::AlignCenter, name);
        });
        QObject::connect(b, &QAbstractButton::clicked, w, [w, pick, i] { pick(i); for (auto* c : w->findChildren<QAbstractButton*>()) c->update(); });
        h->addWidget(b);
    }
    return w;
}

/// A small switch in the accent colour (macOS's .switch at .small).
class Switch : public QAbstractButton {
public:
    Switch(bool on, std::function<void(bool)> changed, QWidget* parent = nullptr) : QAbstractButton(parent), changed_(std::move(changed)) {
        setCheckable(true);
        setChecked(on);
        setFixedSize(32, 18);
        connect(this, &QAbstractButton::toggled, this, [this](bool v) { if (changed_) changed_(v); update(); });
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(isEnabled() ? 1 : 0.4);
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? Theme::accent() : Theme::ink(0.2));
        p.drawRoundedRect(QRectF(rect()), height() / 2.0, height() / 2.0);
        p.setBrush(Qt::white);
        const double d = height() - 4;
        p.drawEllipse(QRectF(isChecked() ? width() - 2 - d : 2, 2, d, d));
    }
private:
    std::function<void(bool)> changed_;
};

/// ToggleRow: a title, a note under it, and a switch.
QWidget* toggleRow(const QString& title, QLabel** note, Switch* sw, bool boxed) {
    auto* row = new QWidget;
    if (boxed) {
        row->setObjectName(QStringLiteral("toggleBox"));
        row->setAttribute(Qt::WA_StyledBackground);
        row->setStyleSheet(QStringLiteral("#toggleBox { background: %1; border-radius: 9px; }").arg(Theme::field().name()));
    }
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(14, 14, 14, 14);
    h->setSpacing(16);
    auto* left = new QVBoxLayout;
    left->setSpacing(3);
    left->addWidget(text(title, 13, Theme::text(), 600));
    *note = text(QString(), 11.5, Theme::textDim(), 400, true);
    left->addWidget(*note);
    h->addLayout(left, 1);
    h->addWidget(sw, 0, Qt::AlignTop);
    return row;
}

/// ThemePreview: the editor in miniature, in a theme's colours.
void drawThemePreview(QPainter& p, const QRectF& r, const Palette& pal) {
    auto c = [](quint32 hex, double a = 1) { return Theme::hex(hex, a); };
    const double w = r.width(), h = r.height(), x0 = r.left(), y0 = r.top();
    const double top = h * 0.11, split = h * 0.56;
    auto fill = [&](double x, double y, double ww, double hh, const QColor& col, double rad = 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawRoundedRect(QRectF(x0 + x, y0 + y, ww, hh), rad, rad);
    };
    fill(0, 0, w, h, c(pal.bg));
    fill(0, 0, w, top, c(pal.panel));
    fill(w * 0.84, top * 0.25, w * 0.12, top * 0.5, c(pal.text), top * 0.25);
    fill(0, top, w * 0.22, split - top, c(pal.panel));
    for (int i = 0; i < 3; ++i) {
        fill(w * 0.025, top + h * 0.06 + i * h * 0.11, w * 0.08, h * 0.07, c(pal.video), 1.5);
        fill(w * 0.12, top + h * 0.085 + i * h * 0.11, w * 0.08, 2, c(pal.textDim, 0.7), 1);
    }
    fill(w * 0.29, top + h * 0.05, w * 0.42, split - top - h * 0.12, Qt::black);
    p.setBrush(c(pal.text));
    p.drawEllipse(QRectF(x0 + w * 0.5 - h * 0.025, y0 + split - h * 0.065, h * 0.05, h * 0.05));
    fill(w * 0.78, top, w * 0.22, split - top, c(pal.panel));
    for (int i = 0; i < 3; ++i) fill(w * 0.81, top + h * 0.08 + i * h * 0.09, w * 0.16, 2, c(pal.ink, 0.12), 1);
    fill(0, split, w, h * 0.07, c(pal.panel));
    for (int i = 0; i < 3; ++i) fill(0, split + h * 0.09 + i * h * 0.12, w, h * 0.1, c(pal.lane));
    auto clip = [&](double x, double y, double ww, quint32 col) {
        fill(x, y, ww, h * 0.1, c(col), 2);
        p.setPen(QPen(c(pal.clipStroke), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(x0 + x + 0.5, y0 + y + 0.5, ww - 1, h * 0.1 - 1), 2, 2);
    };
    for (auto [cx, cw] : {std::pair{0.04, 0.2}, std::pair{0.25, 0.28}, std::pair{0.54, 0.16}}) {
        clip(w * cx, split + h * 0.09, w * cw, pal.video);
        clip(w * cx, split + h * 0.21, w * cw, pal.audio);
    }
    clip(w * 0.04, split + h * 0.33, w * 0.66, pal.audio);
    fill(w * 0.36, split, 1.5, h - split, c(pal.accent));
}

/// ThemeChoice: a theme's picture (Auto: Light on the left, Dark on the right), and its name.
class ThemeChoice : public QAbstractButton {
public:
    ThemeChoice(AppTheme t, std::function<AppTheme()> current, QWidget* parent) : QAbstractButton(parent), theme_(t), current_(std::move(current)) {
        setFixedHeight(56 + 6 + 7 + 16);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool selected = current_() == theme_;
        const QRectF outer(1, 1, width() - 2, 56 + 6 - 2);
        const QRectF pic = outer.adjusted(2, 2, -2, -2);
        QPainterPath clip;
        clip.addRoundedRect(pic, 8, 8);
        p.save();
        p.setClipPath(clip);
        if (theme_ == AppTheme::automatic) {
            drawThemePreview(p, pic, Palette::light);
            p.setClipRect(QRectF(pic.center().x(), pic.top(), pic.width() / 2, pic.height()), Qt::IntersectClip);
            drawThemePreview(p, pic, Palette::dark);
        } else {
            const Palette& pal = theme_ == AppTheme::dark ? Palette::dark : theme_ == AppTheme::grey ? Palette::grey : theme_ == AppTheme::amber ? Palette::amber : Palette::light;
            drawThemePreview(p, pic, pal);
        }
        p.restore();
        p.setPen(QPen(Theme::ink(0.12), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(pic.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        if (selected || underMouse()) {
            p.setPen(QPen(selected ? Theme::accent() : Theme::ink(0.25), 2));
            p.drawRoundedRect(outer, 10, 10);
        }
        p.setFont(Theme::font(11.5, selected ? 600 : 400));
        p.setPen(selected ? Theme::text() : Theme::textDim());
        p.drawText(QRectF(0, outer.bottom() + 7, width(), 16), Qt::AlignHCenter | Qt::AlignTop, Theme::name(theme_));
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    AppTheme theme_;
    std::function<AppTheme()> current_;
};

/// KeyCap: one shortcut, as a key.
QWidget* keyCap(const QString& s) {
    auto* l = new QLabel(s);
    l->setFont(Theme::font(11.5, 500));
    l->setAlignment(Qt::AlignCenter);
    l->setMinimumWidth(24);
    l->setFixedHeight(22);
    l->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid %3; border-radius: 5px; padding: 0 7px; }")
                         .arg(Theme::text().name(), Theme::raised().name(), Theme::ink(0.1).name(QColor::HexArgb)));
    return l;
}

QWidget* scrolled(QWidget* content) {
    auto* s = new QScrollArea;
    s->setFrameShape(QFrame::NoFrame);
    s->setWidgetResizable(true);
    s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    s->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    s->setWidget(content);
    return s;
}

} // namespace

SettingsDialog::SettingsDialog(Controller& c, ShortcutStore& keys, QWidget* parent) : QDialog(parent), c_(c), keys_(keys) {
    setWindowTitle(QStringLiteral("Settings"));
    setFixedSize(600, 640);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    // Its own tab row, like a Mac app's Settings toolbar: icon over name, a soft backing on the chosen one.
    tabRow_ = new QWidget;
    auto* th = new QHBoxLayout(tabRow_);
    th->setContentsMargins(0, 6, 0, 6);
    th->setSpacing(2);
    th->addStretch(1);
    struct T { Tab tab; const char* name; const char* symbol; };
    for (const T& t : {T{Tab::general, "General", "gearshape"}, T{Tab::shortcuts, "Shortcuts", "keyboard"}, T{Tab::magic, "Magic", "wand.and.sparkles"},
                       T{Tab::updates, "Updates", "arrow.down.circle"}}) {
        const Tab tab = t.tab;
        const QString name = QLatin1String(t.name), symbol = QLatin1String(t.symbol);
        const QFont f = Theme::font(13);
        const int w = std::max(48, int(std::ceil(QFontMetricsF(f).horizontalAdvance(name))) + 20);
        auto* b = new Painted(QSize(w, 26 + 3 + 17 + 10), [this, tab, name, symbol, f](QPainter& p, const QRectF& r, bool hovering) {
            const bool selected = tab_ == tab;
            if (selected || hovering) { p.setPen(Qt::NoPen); p.setBrush(Theme::ink(selected ? 0.1 : 0.05)); p.drawRoundedRect(r, 8, 8); }
            const QColor col = selected ? Theme::text() : Theme::textDim();
            Symbols::draw(p, symbol, QPointF(r.center().x(), 5 + 13), 20, col, 400);
            p.setFont(f);
            p.setPen(col);
            p.drawText(QRectF(0, 5 + 26 + 3, r.width(), 17), Qt::AlignHCenter | Qt::AlignTop, name);
        }, tabRow_);
        connect(b, &QAbstractButton::clicked, this, [this, tab] { showTab(tab); });
        th->addWidget(b);
    }
    th->addStretch(1);
    v->addWidget(tabRow_);
    v->addWidget(divider());
    title_ = text(QString(), 20, Theme::text(), 600);
    title_->setContentsMargins(24, 20, 24, 4);
    v->addWidget(title_);
    pages_ = new QStackedWidget;
    pages_->addWidget(general());
    pages_->addWidget(shortcuts());
    pages_->addWidget(magic());
    pages_->addWidget(updates());
    v->addWidget(pages_, 1);
    // General was called Appearance, and may still be the tab kept from then.
    const QString kept = QSettings().value(QStringLiteral("settingsTab"), QStringLiteral("shortcuts")).toString();
    showTab(kept == QStringLiteral("general") || kept == QStringLiteral("appearance") ? Tab::general
         : kept == QStringLiteral("magic") ? Tab::magic : kept == QStringLiteral("updates") ? Tab::updates : Tab::shortcuts);
}

void SettingsDialog::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::panel());
}

void SettingsDialog::showTab(Tab t) {
    tab_ = t;
    recording_.reset();
    static const char* keys[] = {"general", "shortcuts", "magic", "updates"};
    static const char* names[] = {"General", "Shortcuts", "Magic", "Updates"};
    QSettings().setValue(QStringLiteral("settingsTab"), QLatin1String(keys[int(t)]));
    title_->setText(QLatin1String(names[int(t)]));
    pages_->setCurrentIndex(int(t));
    for (auto* b : tabRow_->findChildren<QAbstractButton*>()) b->update();
    if (t == Tab::shortcuts) fillShortcuts();
}

// MARK: General

QWidget* SettingsDialog::general() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(22, 14, 22, 22);
    v->setSpacing(18);
    QVBoxLayout* inner;
    v->addWidget(settingsSection(QStringLiteral("Theme"), &inner));
    {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(10, 12, 10, 10);
        h->setSpacing(10);
        for (AppTheme t : {AppTheme::automatic, AppTheme::dark, AppTheme::grey, AppTheme::amber, AppTheme::light}) {
            auto* choice = new ThemeChoice(t, [] { return Theme::saved(); }, row);
            connect(choice, &QAbstractButton::clicked, this, [this, t, row] {
                Theme::save(t);
                Theme::setCurrent(t);
                for (auto* b : row->findChildren<QAbstractButton*>()) b->update();
                emit themeChanged();
            });
            h->addWidget(choice);
        }
        inner->addWidget(row);
    }
    v->addWidget(settingsSection(QStringLiteral("View"), &inner));
    inner->addWidget(settingsRow(QStringLiteral("Media"), nullptr,
                                 segmented({QStringLiteral("Thumbnails"), QStringLiteral("List")}, [] { return mediaLayout() == MediaLayout::list ? 1 : 0; },
                                           [this](int i) {
                                               setMediaLayout(i == 1 ? MediaLayout::list : MediaLayout::thumbnails);
                                               emit c_.model().mediaSelectionChanged();
                                           })));
    inner->addWidget(divider());
    inner->addWidget(settingsRow(QStringLiteral("Show time as"), nullptr,
                                 segmented({QStringLiteral("00:00:05:15"), QStringLiteral("0:05.50")},
                                           [] { return Timecode::currentStyle() == TimeStyle::simple ? 1 : 0; },
                                           [this](int i) {
                                               const TimeStyle s = i == 1 ? TimeStyle::simple : TimeStyle::timecode;
                                               Timecode::setCurrentStyle(s);
                                               QSettings().setValue(QLatin1String(Timecode::styleKey), i == 1 ? QStringLiteral("simple") : QStringLiteral("timecode"));
                                               emit c_.model().changed();
                                           }, true)));
    v->addWidget(settingsSection(QStringLiteral("Saving"), &inner));
    QLabel* detail = nullptr;
    auto setDetail = [](QLabel* d, bool on) {
        d->setText(on ? QStringLiteral("Saved projects save themselves as you edit.") : QStringLiteral("Projects change only when you save (Ctrl+S)."));
    };
    auto* sw = new Switch(EditorModel::autosaves(), {});
    inner->addWidget(settingsRow(QStringLiteral("Auto-save"), &detail, sw));
    setDetail(detail, EditorModel::autosaves());
    connect(sw, &QAbstractButton::toggled, this, [detail, setDetail](bool on) {
        QSettings().setValue(QLatin1String(EditorModel::autosaveKey), on);
        setDetail(detail, on);
    });
    // Speech: Editroom's speech model (the Mac's comes with macOS; here it's downloaded once, when asked for).
    v->addWidget(settingsSection(QStringLiteral("On this computer"), &inner));
    {
        QLabel* sdetail = nullptr;
        auto* button = new PillButton(QString());
        inner->addWidget(settingsRow(QStringLiteral("Hear what's said in clips"), &sdetail, button));
        auto refresh = [this, sdetail, button] {
            const bool have = Transcripts::available();
            const bool busy = c_.transcripts().downloading();
            sdetail->setText(have ? QStringLiteral("For captions, Magic and searching what's said.")
                             : busy ? QStringLiteral("Downloading…")
                                    : QStringLiteral("About 150 MB, downloaded once. Runs on this computer."));
            button->setText(have ? QStringLiteral("Remove") : QStringLiteral("Download"));
            button->setEnabled(!busy);
        };
        refresh();
        connect(button, &QPushButton::clicked, this, [this, refresh] {
            if (Transcripts::available()) {
                if (Alert::run(this, QStringLiteral("Remove the speech model?"),
                               QStringLiteral("Transcripts already made are kept. New ones need the model downloaded again."),
                               {QStringLiteral("Remove"), QStringLiteral("Cancel")}) == 0) QFile::remove(Transcripts::modelPath());
            } else {
                c_.downloadSpeechModel();
            }
            refresh();
        });
        connect(&c_.transcripts(), &Transcripts::modelInstalled, this, refresh);
        connect(&c_, &Controller::importChanged, this, refresh);
    }
    // What clips show, for Media search and Magic ("pool", "beach"): the Mac's comes with macOS.
    inner->addWidget(divider());
    {
        QLabel* sdetail = nullptr;
        auto* button = new PillButton(QString());
        inner->addWidget(settingsRow(QStringLiteral("See what's in clips"), &sdetail, button));
        auto refresh = [this, sdetail, button] {
            const bool have = SceneTagStore::available(), runtime = SceneTagStore::runtimeAvailable();
            const bool busy = c_.scenes().downloading();
            sdetail->setText(!runtime ? QStringLiteral("Needs ONNX Runtime (libonnxruntime).")
                             : have ? QStringLiteral("So Media search finds a pool, a beach or a dog.")
                             : busy ? QStringLiteral("Downloading…")
                                    : QStringLiteral("About 150 MB, downloaded once. Runs on this computer."));
            button->setText(have ? QStringLiteral("Remove") : QStringLiteral("Download"));
            button->setEnabled(!busy && (runtime || have));
        };
        refresh();
        connect(button, &QPushButton::clicked, this, [this, refresh] {
            if (SceneTagStore::available()) {
                if (Alert::run(this, QStringLiteral("Remove the scene model?"),
                               QStringLiteral("What's already been seen in clips is kept. New clips need the model downloaded again."),
                               {QStringLiteral("Remove"), QStringLiteral("Cancel")}) == 0)
                    for (const QString& f : SceneTagStore::modelFiles()) QFile::remove(SceneTagStore::modelFolder() + u'/' + f);
            } else {
                c_.downloadSceneModel();
            }
            refresh();
        });
        connect(&c_.scenes(), &SceneTagStore::modelInstalled, this, refresh);
        connect(&c_, &Controller::importChanged, this, refresh);
    }
    v->addStretch(1);
    return w;
}

// MARK: Shortcuts

QWidget* SettingsDialog::shortcuts() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* searchRow = new QWidget;
    auto* sr = new QHBoxLayout(searchRow);
    sr->setContentsMargins(24, 12, 24, 4);
    auto* search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("Search shortcuts"));
    search->setFont(Theme::font(12.5));
    search->setFixedHeight(30);
    search->setTextMargins(22, 0, 0, 0);
    search->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: %2; border: none; border-radius: 8px; padding: 0 8px; }")
                              .arg(Theme::field().name(), Theme::text().name()));
    auto* glass = new Painted(QSize(14, 14), [](QPainter& p, const QRectF& r, bool) {
        Symbols::draw(p, QStringLiteral("magnifyingglass"), r.center(), 11, Theme::textFaint(), 500);
    }, search);
    glass->setAttribute(Qt::WA_TransparentForMouseEvents);
    glass->move(10, 8);
    connect(search, &QLineEdit::textChanged, this, [this](const QString& s) { shortcutSearch_ = s; fillShortcuts(); });
    sr->addWidget(search);
    v->addWidget(searchRow);
    auto* list = new QWidget;
    shortcutList_ = new QVBoxLayout(list);
    shortcutList_->setContentsMargins(24, 12, 24, 12);
    shortcutList_->setSpacing(18);
    v->addWidget(scrolled(list), 1);
    v->addWidget(divider());
    auto* foot = new QWidget;
    foot->setFixedHeight(52);
    auto* fh = new QHBoxLayout(foot);
    fh->setContentsMargins(24, 0, 24, 0);
    shortcutNote_ = text(QString(), 11.5, Theme::textDim(), 400, true);
    fh->addWidget(shortcutNote_, 1);
    auto* restore = new PillButton(QStringLiteral("Restore Defaults"));
    restoreDefaults_ = restore;
    connect(restore, &QPushButton::clicked, this, [this] { recording_.reset(); keys_.resetAll(); fillShortcuts(); emit shortcutsChanged(); });
    fh->addWidget(restore);
    v->addWidget(foot);
    return w;
}

void SettingsDialog::setNote(const QString& n) {
    note_ = n;
    fillShortcuts();
    // The note goes after a while.
    QTimer::singleShot(6000, this, [this, n] { if (note_ == n) { note_.clear(); fillShortcuts(); } });
}

void SettingsDialog::fillShortcuts() {
    if (!shortcutList_) return;
    while (QLayoutItem* it = shortcutList_->takeAt(0)) {
        if (it->widget()) { it->widget()->hide(); it->widget()->deleteLater(); }
        delete it;
    }
    const QString q = shortcutSearch_.trimmed().toLower();
    auto matches = [&](ShortcutAction a) {
        if (q.isEmpty()) return true;
        if (Shortcut::title(a).toLower().contains(q)) return true;
        for (const auto& k : keys_.combos(a)) if (k.display().toLower().contains(q)) return true;
        return false;
    };
    // The skimming choice under Play / Pause shows with it, and for searches about skimming.
    bool showsSkim = matches(ShortcutAction::playPause);
    if (!q.isEmpty()) for (const QString& w : {QStringLiteral("skim"), QStringLiteral("playhead"), QStringLiteral("space")}) if (w.startsWith(q) || q.startsWith(w)) showsSkim = true;
    auto skimRow = [] {
        auto* row = new QWidget;
        row->setFixedHeight(34);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(24, 0, 12, 0);
        h->addWidget(text(QStringLiteral("↳  While skimming, play from"), 12, Theme::textDim()));
        h->addStretch(1);
        h->addWidget(segmented({QStringLiteral("Playhead"), QStringLiteral("Skimmer")},
                               [] { return QSettings().value(QLatin1String(EditorModel::playFromSkimmerKey), true).toBool() ? 1 : 0; },
                               [](int i) { QSettings().setValue(QLatin1String(EditorModel::playFromSkimmerKey), i == 1); }));
        return row;
    };
    for (const QString& group : Shortcut::groups()) {
        std::vector<ShortcutAction> actions;
        for (auto a : Shortcut::allCases()) if (Shortcut::group(a) == group && matches(a)) actions.push_back(a);
        const bool skim = group == Shortcut::group(ShortcutAction::playPause) && showsSkim;
        if (actions.empty() && !skim) continue;
        auto* section = new QWidget;
        auto* sv = new QVBoxLayout(section);
        sv->setContentsMargins(0, 0, 0, 0);
        sv->setSpacing(4);
        auto* label = smallCaps(group);
        label->setContentsMargins(10, 0, 0, 0);
        sv->addWidget(label);
        QVBoxLayout* inner;
        sv->addWidget(box(9, false, &inner));
        if (skim && std::find(actions.begin(), actions.end(), ShortcutAction::playPause) == actions.end()) inner->addWidget(skimRow());
        for (auto a : actions) {
            const bool recording = recording_ == a;
            auto* row = new Painted(QSize(10, 34), [recording](QPainter& p, const QRectF& r, bool hovering) {
                if (hovering || recording) { p.setPen(Qt::NoPen); p.setBrush(Theme::ink(0.05)); p.drawRoundedRect(r, 7, 7); }
            });
            row->setMinimumWidth(0);
            row->setMaximumWidth(QWIDGETSIZE_MAX);
            row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(12, 0, 12, 0);
            h->setSpacing(8);
            auto* name = text(Shortcut::title(a), 12.5, Theme::text());
            name->setAttribute(Qt::WA_TransparentForMouseEvents);
            h->addWidget(name);
            if (keys_.isChanged(a)) {
                auto* dot = new Painted(QSize(5, 5), [](QPainter& p, const QRectF& r, bool) { p.setPen(Qt::NoPen); p.setBrush(Theme::accent()); p.drawEllipse(r); });
                dot->setToolTip(QStringLiteral("Changed from the default"));
                h->addWidget(dot);
            }
            h->addStretch(1);
            if (recording) {
                auto* press = new QLabel(QStringLiteral("Press keys…"));
                press->setFont(Theme::font(11.5, 600));
                press->setFixedHeight(22);
                press->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid %3; border-radius: 11px; padding: 0 10px; }")
                                         .arg(Theme::text().name(), alpha(Theme::accent(), 0.25).name(QColor::HexArgb), alpha(Theme::accent(), 0.8).name(QColor::HexArgb)));
                press->setAttribute(Qt::WA_TransparentForMouseEvents);
                h->addWidget(press);
            } else if (keys_.combos(a).empty()) {
                auto* none = text(QStringLiteral("None"), 11.5, Theme::textFaint());
                none->setAttribute(Qt::WA_TransparentForMouseEvents);
                h->addWidget(none);
            } else {
                for (const auto& k : keys_.combos(a)) {
                    auto* cap = keyCap(k.display());
                    cap->setAttribute(Qt::WA_TransparentForMouseEvents);
                    h->addWidget(cap);
                }
            }
            connect(row, &QAbstractButton::clicked, this, [this, a] {
                if (recording_ == a) recording_.reset(); else recording_ = a;
                setFocus();
                fillShortcuts();
            });
            row->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(row, &QWidget::customContextMenuRequested, this, [this, a, row](QPoint at) {
                QMenu menu(this);
                menu.addAction(QStringLiteral("Change Shortcut"), this, [this, a] { recording_ = a; setFocus(); fillShortcuts(); });
                menu.addAction(QStringLiteral("Remove Shortcut"), this, [this, a] { keys_.clear(a); fillShortcuts(); emit shortcutsChanged(); })
                    ->setEnabled(!keys_.combos(a).empty());
                QStringList defaults;
                for (const auto& k : Shortcut::defaults(a)) defaults << k.display();
                menu.addAction(QStringLiteral("Restore Default (%1)").arg(defaults.join(QStringLiteral(", "))), this,
                               [this, a] { keys_.reset(a); fillShortcuts(); emit shortcutsChanged(); })->setEnabled(keys_.isChanged(a));
                menu.exec(row->mapToGlobal(at));
            });
            inner->addWidget(row);
            if (a == ShortcutAction::playPause && skim) inner->addWidget(skimRow());
        }
        shortcutList_->addWidget(section);
    }
    shortcutList_->addStretch(1);
    shortcutNote_->setText(!note_.isEmpty() ? note_ : recording_ ? QStringLiteral("Press the new shortcut, or Esc to cancel.")
                                                                 : QStringLiteral("Click a shortcut to change it. Right-click for more."));
    shortcutNote_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg((!note_.isEmpty() ? Theme::text() : Theme::textDim()).name()));
    bool anyChanged = false;
    for (auto a : Shortcut::allCases()) anyChanged = anyChanged || keys_.isChanged(a);
    restoreDefaults_->setEnabled(anyChanged);
}

void SettingsDialog::keyPressEvent(QKeyEvent* e) {
    if (!recording_) return QDialog::keyPressEvent(e);
    if (e->key() == Qt::Key_Shift || e->key() == Qt::Key_Control || e->key() == Qt::Key_Alt || e->key() == Qt::Key_Meta) return;
    const ShortcutAction a = *recording_;
    recording_.reset();
    // Escape cancels.
    if (e->key() != Qt::Key_Escape) {
        if (auto combo = KeyCombo::fromKey(e->key(), e->text(), int(e->modifiers()))) {
            // A combo given to one action is taken from any other.
            if (auto taken = keys_.set(a, *combo)) {
                const QString t = Shortcut::title(*taken);
                setNote(QStringLiteral("%1 was %2's shortcut, so %2 no longer has it.").arg(combo->display(), t));
            }
            emit shortcutsChanged();
        }
    }
    fillShortcuts();
}

// MARK: Magic

QWidget* SettingsDialog::magic() {
    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(24, 16, 24, 16);
    v->setSpacing(22);
    QSettings s;
    {
        auto* group = new QWidget;
        auto* gv = new QVBoxLayout(group);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->setSpacing(8);
        gv->addWidget(smallCaps(QStringLiteral("Magic")));
        QVBoxLayout* inner;
        gv->addWidget(box(9, false, &inner));
        QLabel* n1;
        QLabel* n2;
        auto* showWork = new Switch(s.value(QStringLiteral("magicShowWork"), true).toBool(), [](bool on) { QSettings().setValue(QStringLiteral("magicShowWork"), on); });
        auto* enable = new Switch(s.value(QStringLiteral("magicEnabled"), true).toBool(), {});
        inner->addWidget(toggleRow(QStringLiteral("Enable Magic"), &n1, enable, false));
        inner->addWidget(divider());
        QWidget* showRow = toggleRow(QStringLiteral("Show Magic at work"), &n2, showWork, false);
        inner->addWidget(showRow);
        n1->setText(QStringLiteral("Magic makes an edit for you: a quick cut on this computer, or one Claude or ChatGPT works out from what you ask. Off hides the Magic button."));
        n2->setText(QStringLiteral("Watch the cursors make the edit. Off, the edit appears at once, as one step you can undo."));
        auto dim = [showRow, showWork](bool on) { showWork->setEnabled(on); showRow->setEnabled(on); showRow->setStyleSheet(on ? QString() : QStringLiteral("QLabel { color: %1; }").arg(Theme::textFaint().name())); };
        dim(enable->isChecked());
        connect(enable, &QAbstractButton::toggled, this, [this, dim](bool on) {
            QSettings().setValue(QStringLiteral("magicEnabled"), on);
            dim(on);
            emit magicEnabledChanged(on);
        });
        v->addWidget(group);
    }
    {
        auto* group = new QWidget;
        auto* gv = new QVBoxLayout(group);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->setSpacing(8);
        gv->addWidget(smallCaps(QStringLiteral("AI apps")));
        QVBoxLayout* inner;
        gv->addWidget(box(9, false, &inner));
        bool first = true;
        for (const AgentApp& app : agentApps()) {
            if (!first) inner->addWidget(divider(58));
            first = false;
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(14, 12, 14, 12);
            h->setSpacing(12);
            auto* icon = new Painted(QSize(32, 32), [](QPainter& p, const QRectF& r, bool) {
                p.setPen(Qt::NoPen);
                p.setBrush(Theme::raised());
                p.drawRoundedRect(r, 7, 7);
                Symbols::draw(p, QStringLiteral("wand.and.sparkles"), r.center(), 14, Theme::textDim(), 500);
            }, row);
            icon->setAttribute(Qt::WA_TransparentForMouseEvents);
            h->addWidget(icon);
            auto* texts = new QVBoxLayout;
            texts->setSpacing(2);
            texts->addWidget(text(app.name, 13, Theme::text(), 600));
            auto* note = text(QString(), 11.5, Theme::textDim(), 400, true);
            texts->addWidget(note);
            h->addLayout(texts, 1);
            auto* button = new PillButton(QString());
            h->addWidget(button);
            auto refresh = [app, note, button] {
                const bool inst = app.installed(), on = inst && app.connected();
                note->setText(!inst ? QStringLiteral("Not on this computer.") : on ? app.connectedNote : app.readyNote);
                note->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textDim().name()));
                button->setText(on ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
                button->setVisible(inst);
            };
            refresh();
            connect(button, &QPushButton::clicked, this, [app, note, button, refresh] {
                QString err;
                const bool wasOn = app.connected();
                // Connecting an app means letting AI apps in.
                if (!wasOn && server().enabled && !server().enabled()) server().setEnabled(true);
                const bool ok = wasOn ? app.disconnect(&err) : app.connect(&err);
                refresh();
                if (!ok) {
                    note->setText(err.isEmpty() ? QStringLiteral("Couldn't connect.") : err);
                    note->setStyleSheet(QStringLiteral("color: #E5735C; background: transparent;"));
                    button->setText(QStringLiteral("Try Again"));
                }
            });
            inner->addWidget(row);
        }
        gv->addWidget(text(QStringLiteral("Connected apps use your own Claude or ChatGPT plan; no API key needed. Disconnecting stops Editroom answering that app, for Magic and for edits you ask for in the app."),
                           11, Theme::textFaint(), 400, true));
        v->addWidget(group);
    }
    {
        auto* group = new QWidget;
        auto* gv = new QVBoxLayout(group);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->setSpacing(8);
        gv->addWidget(smallCaps(QStringLiteral("Editing from other apps")));
        QLabel* note;
        const bool on = server().enabled ? server().enabled() : s.value(QStringLiteral("agentsEnabled"), true).toBool();
        auto* sw = new Switch(on, {});
        gv->addWidget(toggleRow(QStringLiteral("Let AI apps edit in Editroom"), &note, sw, true));
        auto* problem = text(QString(), 11.5, QColor(0xE5, 0x73, 0x5C), 400, true);
        gv->addWidget(problem);
        auto* working = text(QString(), 11.5, Theme::textDim());
        gv->addWidget(working);
        auto refresh = [note, problem, working, sw] {
            const bool enabled = sw->isChecked();
            note->setText(enabled ? QStringLiteral("On: connected apps can edit here, with their own cursor, and every change can be undone. Only apps on this computer can reach Editroom.")
                                  : QStringLiteral("Off: no app can reach Editroom, and Magic only makes quick cuts on this computer."));
            const QString p = server().problem ? server().problem() : QString();
            problem->setText(p);
            problem->setVisible(enabled && !p.isEmpty());
            // Only agents that have done something in the last minute.
            const QStringList active = server().active ? server().active() : QStringList();
            working->setText(QStringLiteral("Working now: ") + active.join(QStringLiteral(", ")));
            working->setVisible(!active.isEmpty());
        };
        refresh();
        connect(sw, &QAbstractButton::toggled, this, [refresh](bool v2) {
            QSettings().setValue(QStringLiteral("agentsEnabled"), v2);
            if (server().setEnabled) server().setEnabled(v2);
            refresh();
        });
        auto* timer = new QTimer(group);
        timer->setInterval(5000);
        connect(timer, &QTimer::timeout, group, refresh);
        timer->start();
        v->addWidget(group);
    }
    v->addStretch(1);
    return scrolled(content);
}

// MARK: Updates

QWidget* SettingsDialog::updates() {
    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(24, 16, 24, 16);
    v->setSpacing(16);
    auto card = [](const QString& title, QVBoxLayout** inner) {
        auto* w = new QWidget;
        w->setObjectName(QStringLiteral("card"));
        w->setAttribute(Qt::WA_StyledBackground);
        w->setStyleSheet(QStringLiteral("#card { background: %1; border-radius: 12px; }").arg(Theme::field().name()));
        auto* cv = new QVBoxLayout(w);
        cv->setContentsMargins(16, 16, 16, 16);
        cv->setSpacing(12);
        cv->addWidget(text(title, 13, Theme::text(), 600));
        *inner = cv;
        return w;
    };
    auto row = [](const QString& name, const QString& value) {
        auto* r = new QWidget;
        auto* h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        h->addWidget(text(name, 12.5, Theme::textDim()));
        auto* val = text(value, 12.5, Theme::text());
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        h->addWidget(val);
        h->addStretch(1);
        return r;
    };
    QVBoxLayout* inner;
    v->addWidget(card(QStringLiteral("Software Update"), &inner));
    inner->addWidget(row(QStringLiteral("Installed version"), QStringLiteral("%1 (%2)").arg(QCoreApplication::applicationVersion(), QStringLiteral("LX"))));
    auto lastText = [] {
        const auto last = Updates::shared().lastCheck();
        return last ? QLocale().toString(last->toLocalTime(), QLocale::ShortFormat) : QStringLiteral("Never");
    };
    auto* lastRow = row(QStringLiteral("Last checked"), lastText());
    inner->addWidget(lastRow);
    inner->addWidget(divider());
    // Automatic checks: off until turned on (nothing goes online unless asked); then once a day, at startup.
    auto* automatic = new QWidget;
    auto* ah = new QHBoxLayout(automatic);
    ah->setContentsMargins(0, 0, 0, 0);
    auto* sw = new Switch(Updates::shared().automatic(), [](bool on) { Updates::shared().setAutomatic(on); });
    ah->addWidget(sw);
    ah->addWidget(text(QStringLiteral("Check for updates automatically"), 12, Theme::text()));
    ah->addStretch(1);
    inner->addWidget(automatic);
    auto* check = new PillButton(QStringLiteral("Check for Updates…"));
    inner->addWidget(check, 0, Qt::AlignLeft);
    auto* status = text(QStringLiteral("Checks the latest release on GitHub."), 11, Theme::textFaint());
    inner->addWidget(status);
    connect(check, &QPushButton::clicked, this, [this, check, status] {
        check->setEnabled(false);
        status->setText(QStringLiteral("Checking…"));
        Updates::shared().checkAndTell(this);
    });
    connect(&Updates::shared(), &Updates::checked, this, [check, status, lastRow, lastText](const Updates::Result& r) {
        check->setEnabled(true);
        status->setText(!r.ok ? QStringLiteral("Couldn't check: %1").arg(r.error)
                        : r.none ? QStringLiteral("No releases on GitHub yet.")
                        : r.newer ? QStringLiteral("Editroom %1 is available.").arg(r.version)
                                  : QStringLiteral("You're up to date."));
        if (auto* l = lastRow->findChildren<QLabel*>().value(1)) l->setText(lastText());
    });
    v->addWidget(card(QStringLiteral("Release Notes"), &inner));
    inner->addWidget(text(QStringLiteral("Each update lists what's new before you install it. Every version's notes are on GitHub."), 12, Theme::textDim(), 400, true));
    auto* releases = new PillButton(QStringLiteral("View GitHub Releases"));
    connect(releases, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(Updates::releasesURL())); });
    inner->addWidget(releases, 0, Qt::AlignLeft);
    v->addStretch(1);
    return scrolled(content);
}

} // namespace er
