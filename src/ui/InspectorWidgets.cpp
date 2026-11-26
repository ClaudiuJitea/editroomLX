#include "InspectorWidgets.h"
#include "Symbols.h"
#include "Theme.h"
#include "editor/EditorModel.h"
#include <QApplication>
#include <QColorDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace er {

namespace {

QColor toColor(const RGBA& c) { return QColor::fromRgbF(float(c.r), float(c.g), float(c.b), float(c.a)); }
RGBA fromColor(const QColor& q) {
    const QColor c = q.toRgb();
    return RGBA{c.redF(), c.greenF(), c.blueF(), c.alphaF(), {}};
}

/// Mouse-over state for the hand-drawn buttons.
bool hovered(const QWidget* w) { return w->underMouse(); }

} // namespace

int rowLabelWidth() {
    // The Mac's 52 px fits its longest names ("Exposure", "Fade Out") in SF Pro; Inter is wider, so the column is
    // as wide as those names need here, and never narrower than the Mac's.
    static const int w = [] {
        const QFontMetricsF fm(Theme::font(11.5));
        double most = 52;
        for (const char* n : {"Exposure", "Fade Out", "Fade In", "Intensity", "Position", "Saturate", "Contrast", "Volume", "Length"})
            most = std::max(most, fm.horizontalAdvance(QLatin1String(n)) + 1);
        return int(std::ceil(most));
    }();
    return w;
}

QString tenths(double v) {
    const double t = std::round(v * 10) / 10;
    return t == std::round(t) ? QString::number(qint64(t)) : QString::asprintf("%.1f", t);
}

QLabel* note(const QString& text, double px, int indent) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setFont(Theme::font(px));
    l->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textFaint().name(QColor::HexArgb)));
    if (indent) l->setContentsMargins(indent, 0, 0, 0);
    return l;
}

// MARK: InspectorSection

/// The header: chevron, title in small caps, a dot while folded with something changed, and the action.
class SectionHeader : public QWidget {
public:
    SectionHeader(InspectorSection* s) : QWidget(s), s_(s) {
        setFixedHeight(16);
        setCursor(Qt::ArrowCursor);
        setMouseTracking(true);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor c = underMouse() ? Theme::textDim() : Theme::textFaint();
        // Chevron: right when folded, down when open.
        p.save();
        p.translate(4, height() / 2.0);
        if (!s_->collapsed_) p.rotate(90);
        Symbols::draw(p, QStringLiteral("chevron.right"), QPointF(0, 0), 7.5, c, 700);
        p.restore();
        QFont f = Theme::label();
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
        p.setFont(f);
        p.setPen(c);
        const QString t = s_->title_.toUpper();
        p.drawText(QRectF(14, 0, width() - 14, height()), Qt::AlignVCenter | Qt::AlignLeft, t);
        if (s_->marked_ && s_->collapsed_) {
            const double x = 14 + QFontMetricsF(f).horizontalAdvance(t) + 6 + 2.5;
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::accent());
            p.drawEllipse(QPointF(x, height() / 2.0), 2.5, 2.5);
        }
    }
    void mousePressEvent(QMouseEvent*) override { s_->toggle(); }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    InspectorSection* s_;
};

/// The Mac's TextLink: a word that acts, accent on hover.
class TextLink : public QAbstractButton {
public:
    TextLink(const QString& t, QWidget* parent) : QAbstractButton(parent) {
        setText(t);
        setCursor(Qt::ArrowCursor);
        setFont(Theme::font(11.5, 500));
    }
    QSize sizeHint() const override { return {QFontMetrics(font()).horizontalAdvance(text()) + 2, 16}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setFont(font());
        p.setPen(underMouse() ? Theme::accent() : Theme::textDim());
        p.drawText(rect(), Qt::AlignVCenter | Qt::AlignRight, text());
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
};

InspectorSection::InspectorSection(const QString& title, std::optional<Action> action, bool collapsedByDefault, bool marked, QWidget* parent)
    : QWidget(parent), title_(title), marked_(marked) {
    setProperty("title", title);
    // As tall as its rows, never stretched (a text field inside would otherwise take the panel's spare height).
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    const QVariant saved = QSettings().value(QStringLiteral("inspector.collapsed.") + title);
    collapsed_ = saved.isValid() ? saved.toBool() : collapsedByDefault;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);
    header_ = new SectionHeader(this);
    header_->setObjectName(QStringLiteral("sectionHeader"));
    top->addWidget(header_, 1);
    if (action) {
        action_ = new TextLink(action->title, this);
        connect(action_, &QAbstractButton::clicked, this, [run = action->run] { QTimer::singleShot(0, run); });
        top->addWidget(action_);
    }
    v->addLayout(top);
    content_ = new QWidget(this);
    body_ = new QVBoxLayout(content_);
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(6);
    v->addWidget(content_);
    sync();
}

void InspectorSection::add(QWidget* w) { body_->addWidget(w); }
void InspectorSection::add(QLayout* l) { body_->addLayout(l); }

void InspectorSection::toggle() {
    collapsed_ = !collapsed_;
    QSettings().setValue(QStringLiteral("inspector.collapsed.") + title_, collapsed_);
    sync();
}

void InspectorSection::sync() {
    content_->setVisible(!collapsed_);
    if (action_) action_->setVisible(!collapsed_);
    header_->update();
}

// MARK: ThinSlider

ThinSlider::ThinSlider(QWidget* parent) : QWidget(parent) {
    setFixedHeight(20);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ThinSlider::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double knob = 13, travel = std::max(1.0, width() - knob);
    const double x = std::clamp(value_, 0.0, 1.0) * travel;
    const double cy = height() / 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::ink(0.12));
    p.drawRoundedRect(QRectF(0, cy - 1, width(), 2), 1, 1);
    p.setBrush(Theme::text());
    p.drawRoundedRect(QRectF(0, cy - 1, x + knob / 2, 2), 1, 1);
    const double r = (knob / 2) * (dragging_ || hovering_ ? 1.12 : 1);
    // The knob, with a 3 px ring in the panel colour around it.
    p.setBrush(Theme::panel());
    p.drawEllipse(QPointF(x + knob / 2, cy), r + 3, r + 3);
    p.setBrush(Theme::text());
    p.drawEllipse(QPointF(x + knob / 2, cy), r, r);
}

void ThinSlider::drag(double x) {
    const double knob = 13, travel = std::max(1.0, width() - knob);
    const double n = std::clamp((x - knob / 2) / travel, 0.0, 1.0);
    if (onChange) onChange(n);
}

void ThinSlider::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    if (onEditing) onEditing(true);
    drag(e->position().x());
    update();
}

void ThinSlider::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) drag(e->position().x());
}

void ThinSlider::mouseReleaseEvent(QMouseEvent*) {
    if (!dragging_) return;
    dragging_ = false;
    update();
    if (onEditing) onEditing(false);
}

// MARK: NumberField

NumberField::NumberField(QWidget* parent) : QLineEdit(parent) {
    setFrame(false);
    setFont(Theme::mono(11.5));
    setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; border: none; padding: 0 6px; }")
                      .arg(Theme::text().name()));
}

void NumberField::keyPressEvent(QKeyEvent* e) {
    const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if ((e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) && plain) {
        if (onStep) onStep(e->key() == Qt::Key_Up, e->modifiers() & Qt::ShiftModifier);
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        cancelled_ = true;
        if (onCancel) onCancel();
        clearFocus();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        clearFocus();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

void NumberField::focusInEvent(QFocusEvent* e) {
    cancelled_ = false;
    QLineEdit::focusInEvent(e);
    emit focusChanged(true);
}

void NumberField::focusOutEvent(QFocusEvent* e) {
    QLineEdit::focusOutEvent(e);
    if (!cancelled_ && onCommit) onCommit();
    cancelled_ = false;
    emit focusChanged(false);
}

void NumberField::paintEvent(QPaintEvent* e) { QLineEdit::paintEvent(e); }

// MARK: RowIconView

RowIconView::RowIconView(RowIcon icon, QWidget* parent) : QWidget(parent), icon_(std::move(icon)), color_(Theme::textDim()) {
    setFixedSize(18, 18);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void RowIconView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(width() / 2.0, height() / 2.0);
    switch (icon_.kind) {
    case RowIcon::Kind::symbol: Symbols::draw(p, icon_.name, c, 11, color_, 500); break;
    case RowIcon::Kind::fadeIn:
    case RowIcon::Kind::fadeOut: {
        // An arrow and a bar: → | for in, | → for out.
        const bool in = icon_.kind == RowIcon::Kind::fadeIn;
        const double arrowX = in ? c.x() - 2 : c.x() + 2.5;
        Symbols::draw(p, QStringLiteral("arrow.right"), QPointF(arrowX, c.y()), 9, color_, 600);
        p.setPen(Qt::NoPen);
        p.setBrush(color_);
        const double barX = in ? c.x() + 3.8 : c.x() - 4.6;
        p.drawRoundedRect(QRectF(barX, c.y() - 5, 1.5, 10), 0.75, 0.75);
        break;
    }
    }
}

// MARK: ParamRow

ParamRow::ParamRow(Spec spec, QWidget* parent) : QWidget(parent), s_(std::move(spec)) {
    setFixedHeight(28);
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    h->addWidget(new RowIconView(s_.icon));
    auto* label = new QLabel(s_.label);
    label->setFont(Theme::font(11.5));
    label->setFixedWidth(rowLabelWidth());
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::text().name()));
    h->addWidget(label);
    slider_ = new ThinSlider;
    slider_->setValue(normalised());
    slider_->onEditing = s_.onEditing;
    slider_->onChange = [this](double n) { s_.onChange(denormalise(n)); };
    h->addWidget(slider_, 1);
    // Always a text field, so Tab moves to the next row's value like any form.
    auto* box = new QWidget;
    box->setFixedSize(68, 26);
    box->setObjectName(QStringLiteral("paramBox"));
    box->setAttribute(Qt::WA_StyledBackground);
    auto styleBox = [box](bool focused) {
        box->setStyleSheet(QStringLiteral("#paramBox { background: %1; border-radius: 7px; border: 1px solid %2; }")
                               .arg(Theme::field().name(), focused ? Theme::text().name() : QStringLiteral("transparent")));
    };
    styleBox(false);
    auto* bh = new QHBoxLayout(box);
    bh->setContentsMargins(0, 0, s_.inside ? 4 : 0, 0);
    bh->setSpacing(4);
    field_ = new NumberField;
    field_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // With a button inside, the number runs up to it (no right padding), as on the Mac.
    if (s_.inside) field_->setTextMargins(2, 0, 0, 0);
    if (s_.inside) field_->setStyleSheet(field_->styleSheet().replace(QStringLiteral("padding: 0 6px;"), QStringLiteral("padding: 0;")));
    field_->setText(s_.format(s_.value));
    field_->onStep = [this](bool up, bool big) { nudge(up, big); };
    field_->onCommit = [this] { commit(); field_->setText(s_.format(s_.value)); };
    field_->onCancel = [this] { field_->setText(s_.format(s_.value)); };
    connect(field_, &NumberField::focusChanged, box, styleBox);
    bh->addWidget(field_, 1);
    if (s_.inside) bh->addWidget(s_.inside);
    h->addWidget(box);
    if (s_.trailing) h->addWidget(s_.trailing);
}

double ParamRow::normalised() const {
    const double v = std::clamp(s_.value, s_.lo, s_.hi);
    if (s_.logarithmic) return (std::log(v) - std::log(s_.lo)) / (std::log(s_.hi) - std::log(s_.lo));
    return (v - s_.lo) / (s_.hi - s_.lo);
}

double ParamRow::denormalise(double n) const {
    if (s_.logarithmic) return std::exp(std::log(s_.lo) + n * (std::log(s_.hi) - std::log(s_.lo)));
    return s_.lo + n * (s_.hi - s_.lo);
}

void ParamRow::setValue(double v) {
    s_.value = v;
    slider_->setValue(normalised());
    if (!field_->hasFocus()) field_->setText(s_.format(v));
}

std::optional<double> ParamRow::parse(const QString& s) {
    QString cleaned, text = s;
    text.replace(u',', u'.').replace(QChar(0x2212), u'-');
    for (QChar ch : text)
        if (QStringLiteral("0123456789.-").contains(ch)) cleaned += ch;
    bool ok = false;
    const double v = cleaned.toDouble(&ok);
    return ok ? std::optional<double>(v) : std::nullopt;
}

/// ↑/↓ while typing: the value up or down a step, by what the field shows (1 for whole numbers, 0.1 for
/// decimals; ten times that with Shift). What's been typed counts as the starting point.
void ParamRow::nudge(bool up, bool big) {
    const double start = parse(field_->text()).value_or(s_.value);
    const double step = s_.step.value_or(s_.format(s_.value).contains(u'.') ? 0.1 : 1) * (big ? 10 : 1);
    // To the next whole step, so 0.47 goes to 0.5, not 0.57.
    const double n = (up ? std::floor(start / step) : std::ceil(start / step)) + (up ? 1 : -1);
    const double v = std::clamp(std::round(n * step * 1e6) / 1e6, s_.lo, s_.hi);
    if (s_.onEditing) s_.onEditing(true);
    s_.onChange(v);
    if (s_.onEditing) s_.onEditing(false);
    s_.value = v;
    field_->setText(s_.format(v));
}

/// Reads the number out of whatever is in the field ("100%", "0.50s", "−18.8 dB" all work).
void ParamRow::commit() {
    if (field_->text() == s_.format(s_.value)) return;
    if (auto v = parse(field_->text())) {
        if (s_.onEditing) s_.onEditing(true);
        s_.onChange(std::clamp(*v, s_.lo, s_.hi));
        if (s_.onEditing) s_.onEditing(false);
    }
}

// MARK: CurveMenu

namespace {
void drawCurve(QPainter& p, FadeCurve curve, bool rising, QRectF r, const QColor& color, int steps) {
    QPainterPath path;
    for (int i = 0; i <= steps; ++i) {
        const double x = double(i) / steps;
        const double y = applyFadeCurve(curve, rising ? x : 1 - x);
        const QPointF pt(r.left() + x * r.width(), r.top() + (1 - y) * r.height());
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}
} // namespace

CurveMenu::CurveMenu(FadeCurve selection, bool rising, bool active, std::function<void(FadeCurve)> onPick, QWidget* parent)
    : QAbstractButton(parent), selection_(selection), rising_(rising), active_(active), onPick_(std::move(onPick)) {
    setFixedSize(48, 26);
    setToolTip(fadeCurveLabel(selection));
    connect(this, &QAbstractButton::clicked, this, [this] {
        // The current one is ticked and opens under the mouse, like a pop-up button.
        QMenu menu;
        QAction* current = nullptr;
        for (FadeCurve c : {FadeCurve::linear, FadeCurve::easeIn, FadeCurve::easeOut, FadeCurve::smooth}) {
            QAction* a = menu.addAction(glyph(c, rising_, palette().color(QPalette::Text)), QStringLiteral("   ") + fadeCurveLabel(c));
            a->setCheckable(true);
            a->setChecked(c == selection_);
            if (c == selection_) current = a;
            connect(a, &QAction::triggered, this, [this, c] { onPick_(c); });
        }
        menu.exec(QCursor::pos(), current);
    });
}

QIcon CurveMenu::glyph(FadeCurve c, bool rising, const QColor& color) {
    QPixmap pm(44, 26);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    drawCurve(p, c, rising, QRectF(1.5, 1.5, 22 - 3, 13 - 3), color, 24);
    p.end();
    return QIcon(pm);
}

void CurveMenu::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(hovered(this) ? Theme::hover() : Theme::field());
    p.drawRoundedRect(QRectF(rect()), 7, 7);
    // The curve (20 × 12) then the chevron, centred together with 5 px between.
    const double total = 20 + 5 + 7;
    const double x0 = (width() - total) / 2;
    drawCurve(p, selection_, rising_, QRectF(x0 + 1, (height() - 12) / 2.0 + 1, 18, 10), active_ ? Theme::accent() : Theme::textDim(), 20);
    Symbols::draw(p, QStringLiteral("chevron.down"), QPointF(x0 + 25 + 3.5, height() / 2.0), 7.5, Theme::textDim(), 700);
}

// MARK: Chip

Chip::Chip(const QString& label, bool active, std::function<void()> action, QWidget* parent) : QAbstractButton(parent), active_(active) {
    setText(label);
    setFont(Theme::font(10, 500));
    setFixedHeight(26);
    connect(this, &QAbstractButton::clicked, this, [action = std::move(action)] { action(); });
}

QSize Chip::sizeHint() const { return {QFontMetrics(font()).horizontalAdvance(text()) + 20, 26}; }

void Chip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    QColor fill = active_ ? Theme::accent() : hovered(this) ? Theme::hover() : Theme::field();
    if (active_) fill.setAlphaF(0.16);
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(rect()), 7, 7);
    p.setFont(font());
    p.setPen(active_ ? Theme::accent() : hovered(this) ? Theme::text() : Theme::textDim());
    p.drawText(rect(), Qt::AlignCenter, text());
}

// MARK: MenuField

MenuField::MenuField(const QString& label, int height, QWidget* parent)
    : QAbstractButton(parent), label_(label), height_(height), menu_(new QMenu(this)) {
    setFixedHeight(height);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QAbstractButton::pressed, this, [this] {
        menu_->setMinimumWidth(width());
        menu_->exec(mapToGlobal(QPoint(0, this->height() + 2)));
        setDown(false);
        update();
    });
}

void MenuField::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(hovered(this) ? Theme::hover() : Theme::field());
    p.setPen(QPen(Theme::ink(0.05), 1));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.setFont(Theme::font(11.5));
    p.setPen(Theme::text());
    const QRectF tr(11, 0, width() - 11 - 11 - 8 - 4, height());
    p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(Theme::font(11.5)).elidedText(label_, Qt::ElideMiddle, int(tr.width())));
    Symbols::draw(p, QStringLiteral("chevron.down"), QPointF(width() - 11 - 4, height() / 2.0), 8.5, Theme::textDim(), 700);
}

// MARK: FontPicker

namespace {
/// The searchable list of families, in a popover beside the field.
class FontList : public QFrame {
public:
    FontList(const QString& selection, std::function<void(const QString&)> onPick, QWidget* parent)
        : QFrame(parent, Qt::Popup), onPick_(std::move(onPick)) {
        setFixedSize(260, 360);
        setObjectName(QStringLiteral("fontList"));
        setStyleSheet(QStringLiteral("#fontList { background: %1; border: 1px solid %2; border-radius: 8px; }")
                          .arg(Theme::panel().name(), Theme::stroke().name(QColor::HexArgb)));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        auto* top = new QWidget;
        top->setFixedHeight(34);
        auto* th = new QHBoxLayout(top);
        th->setContentsMargins(10, 0, 10, 0);
        th->setSpacing(6);
        auto* glass = new QLabel;
        glass->setPixmap(Symbols::icon(QStringLiteral("magnifyingglass"), 11, Theme::textFaint()).pixmap(16, 16));
        th->addWidget(glass);
        query_ = new QLineEdit;
        query_->setPlaceholderText(QStringLiteral("Search fonts"));
        query_->setFrame(false);
        query_->setFont(Theme::font(11.5));
        query_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        th->addWidget(query_, 1);
        v->addWidget(top);
        auto* line = new QFrame;
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::stroke().name(QColor::HexArgb)));
        v->addWidget(line);
        list_ = new QListWidget;
        list_->setFrameShape(QFrame::NoFrame);
        list_->setStyleSheet(QStringLiteral("QListWidget { background: transparent; padding: 4px; } QListWidget::item { height: 28px; padding: 0 8px; border-radius: 6px; }"
                                            "QListWidget::item:hover, QListWidget::item:selected { background: %1; color: %2; }")
                                 .arg(Theme::hover().name(QColor::HexArgb), Theme::text().name()));
        v->addWidget(list_, 1);
        families_ = QStringList{QString()} + QFontDatabase::families();
        fill(selection);
        connect(query_, &QLineEdit::textChanged, this, [this, selection] { fill(selection); });
        connect(query_, &QLineEdit::returnPressed, this, [this] { if (list_->count() && list_->item(0)->data(Qt::UserRole).isValid()) pick(list_->item(0)); });
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { pick(it); });
        query_->setFocus();
    }
private:
    void fill(const QString& selection) {
        list_->clear();
        const QString q = query_->text().trimmed();
        QListWidgetItem* current = nullptr;
        for (const QString& fam : families_) {
            const QString shown = fam.isEmpty() ? QStringLiteral("System") : fam;
            if (!q.isEmpty() && !shown.contains(q, Qt::CaseInsensitive)) continue;
            auto* it = new QListWidgetItem(shown, list_);
            it->setData(Qt::UserRole, fam);
            QFont f = fam.isEmpty() ? Theme::font(14) : QFont(fam);
            f.setPixelSize(14);
            it->setFont(f);
            if (fam == selection) {
                it->setIcon(Symbols::icon(QStringLiteral("checkmark"), 10, Theme::accent(), 700));
                current = it;
            }
        }
        if (list_->count() == 0) {
            auto* it = new QListWidgetItem(QStringLiteral("No fonts match"), list_);
            it->setFlags(Qt::NoItemFlags);
            it->setFont(Theme::font(11.5));
            it->setForeground(Theme::textFaint());
        }
        if (current && q.isEmpty()) list_->scrollToItem(current, QAbstractItemView::PositionAtCenter);
    }
    void pick(QListWidgetItem* it) {
        const QVariant v = it->data(Qt::UserRole);
        if (!v.isValid()) return;
        onPick_(v.toString());
        close();
    }
    std::function<void(const QString&)> onPick_;
    QLineEdit* query_;
    QListWidget* list_;
    QStringList families_;
};
} // namespace

FontPicker::FontPicker(const QString& selection, std::function<void(const QString&)> onPick, QWidget* parent)
    : QAbstractButton(parent), selection_(selection), onPick_(std::move(onPick)) {
    setFixedHeight(28);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QAbstractButton::clicked, this, &FontPicker::open);
}

void FontPicker::open() {
    auto* list = new FontList(selection_, onPick_, this);
    list->setAttribute(Qt::WA_DeleteOnClose);
    // Beside the field, on its left (the Mac's popover's arrow is on the list's leading edge).
    QPoint at = mapToGlobal(QPoint(-list->width() - 8, height() / 2 - list->height() / 2));
    if (const QScreen* s = screen()) {
        const QRect g = s->availableGeometry();
        at.setX(std::clamp(at.x(), g.left(), g.right() - list->width()));
        at.setY(std::clamp(at.y(), g.top(), g.bottom() - list->height()));
    }
    list->move(at);
    list->show();
}

void FontPicker::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(hovered(this) ? Theme::hover() : Theme::field());
    p.setPen(QPen(Theme::ink(0.05), 1));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.setFont(Theme::font(11.5));
    p.setPen(Theme::text());
    p.drawText(QRectF(11, 0, width() - 34, height()), Qt::AlignVCenter | Qt::AlignLeft, selection_.isEmpty() ? QStringLiteral("System") : selection_);
    Symbols::draw(p, QStringLiteral("chevron.down"), QPointF(width() - 15, height() / 2.0), 8.5, Theme::textDim(), 700);
}

// MARK: ColorField

ColorField::ColorField(RGBA color, std::function<void(RGBA)> onChange, QWidget* parent)
    : QAbstractButton(parent), color_(std::move(color)), onChange_(std::move(onChange)) {
    setFixedHeight(24);
    setMinimumWidth(44);
    connect(this, &QAbstractButton::clicked, this, [this] {
        QColorDialog dlg(toColor(color_), window());
        dlg.setOption(QColorDialog::NoButtons, false);
        const RGBA before = color_;
        connect(&dlg, &QColorDialog::currentColorChanged, this, [this](const QColor& c) { color_ = fromColor(c); color_.a = 1; onChange_(color_); update(); });
        if (dlg.exec() != QDialog::Accepted) { color_ = before; onChange_(before); update(); }
    });
}

void ColorField::paintEvent(QPaintEvent*) {
    // The macOS colour well: a rounded frame with the colour inside.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(0, 0, std::min(44, width()), height()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(Theme::ink(0.18), 1));
    p.setBrush(Theme::field());
    p.drawRoundedRect(r, 6, 6);
    p.setPen(Qt::NoPen);
    p.setBrush(toColor(color_));
    p.drawRoundedRect(r.adjusted(3, 3, -3, -3), 4, 4);
}

// MARK: FrameSizeFields

FrameSizeFields::FrameSizeFields(int width, int height, std::function<void(int, int)> apply, QWidget* parent)
    : QWidget(parent), width_(width), height_(height), apply_(std::move(apply)) {
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto make = [this](bool isWidth) {
        auto* box = new QWidget;
        box->setFixedHeight(28);
        box->setObjectName(QStringLiteral("sizeBox"));
        box->setAttribute(Qt::WA_StyledBackground);
        auto style = [box](bool focused) {
            box->setStyleSheet(QStringLiteral("#sizeBox { background: %1; border-radius: 8px; border: 1px solid %2; }")
                                   .arg(Theme::field().name(), focused ? Theme::text().name() : QStringLiteral("transparent")));
        };
        style(false);
        auto* l = new QHBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        auto* f = new NumberField;
        f->setAlignment(Qt::AlignCenter);
        connect(f, &NumberField::focusChanged, box, style);
        f->onCommit = [this] { commit(); };
        f->onCancel = [this] { sync(); };
        f->onStep = [this, f, isWidth](bool up, bool big) {
            QString digits;
            for (QChar c : f->text()) if (c.isDigit()) digits += c;
            bool ok = false;
            const int was = digits.toInt(&ok);
            const int n = std::clamp((ok ? was : (isWidth ? width_ : height_)) / 2 * 2 + (up ? 2 : -2) * (big ? 10 : 1), 16, 8192);
            f->setText(QString::number(n));
            commit();
        };
        l->addWidget(f);
        return std::make_pair(box, f);
    };
    auto [wb, wf] = make(true);
    auto [hb, hf] = make(false);
    w_ = wf;
    h_ = hf;
    h->addWidget(wb, 1);
    auto* x = new QLabel(QStringLiteral("×"));
    x->setFont(Theme::font(11.5));
    x->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textFaint().name()));
    h->addWidget(x);
    h->addWidget(hb, 1);
    sync();
}

void FrameSizeFields::set(int width, int height) {
    width_ = width;
    height_ = height;
    if (!w_->hasFocus() && !h_->hasFocus()) sync();
}

void FrameSizeFields::sync() {
    w_->setText(QString::number(width_));
    h_->setText(QString::number(height_));
}

void FrameSizeFields::commit() {
    auto digits = [](const QString& s) { QString d; for (QChar c : s) if (c.isDigit()) d += c; return d; };
    bool okW = false, okH = false;
    const int nw = digits(w_->text()).toInt(&okW), nh = digits(h_->text()).toInt(&okH);
    if (!okW || !okH) return sync();
    // Encoders need even dimensions.
    const int cw = std::min(8192, std::max(16, nw / 2 * 2)), ch = std::min(8192, std::max(16, nh / 2 * 2));
    if (cw != width_ || ch != height_) apply_(cw, ch); else sync();
}

// MARK: CaptionEye

CaptionEye::CaptionEye(bool on, std::function<void()> action, QWidget* parent) : QAbstractButton(parent), on_(on) {
    setFixedSize(20, 20);
    setToolTip(on ? QStringLiteral("Hide") : QStringLiteral("Show"));
    connect(this, &QAbstractButton::clicked, this, [action = std::move(action)] { action(); });
}

void CaptionEye::paintEvent(QPaintEvent*) {
    QPainter p(this);
    QColor c = on_ ? Theme::textDim() : Theme::textFaint();
    if (hovered(this)) c = c.lighter(115);
    Symbols::draw(p, on_ ? QStringLiteral("eye") : QStringLiteral("eye.slash"), QPointF(10, 10), 9.5, c, 600);
}

// MARK: CaptionTextField

CaptionTextField::CaptionTextField(EditorModel& m, CaptionBlock block, QWidget* parent)
    : QPlainTextEdit(parent), m_(m), block_(std::move(block)) {
    setFont(Theme::font(12, 500));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTabChangesFocus(true);
    setStyleSheet(QStringLiteral("QPlainTextEdit { background: %1; color: %2; border-radius: 8px; padding: 3px 4px; }")
                      .arg(Theme::field().name(), Theme::text().name()));
    settingText_ = true;
    setPlainText(block_.text());
    settingText_ = false;
    fit();
    // Shown in the viewer as it's typed (the playhead goes to the caption if it's elsewhere).
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        fit();
        if (settingText_ || !hasFocus() || !editing_) return;
        m_.typeCaptionText(*editing_, toPlainText());
    });
    connect(&m_, &EditorModel::captionEditRequested, this, &CaptionTextField::takeRequest);
    QTimer::singleShot(0, this, &CaptionTextField::takeRequest);
}

/// One to four lines, as tall as the words need.
void CaptionTextField::fit() {
    const QFontMetrics fm(font());
    const int lines = std::clamp(int(document()->size().height()), 1, 4);
    setFixedHeight(lines * fm.lineSpacing() + 14 + 2);
}

void CaptionTextField::setBlock(const CaptionBlock& b) {
    block_ = b;
    if (!hasFocus()) {
        settingText_ = true;
        setPlainText(b.text());
        settingText_ = false;
    }
}

void CaptionTextField::takeRequest() {
    if (m_.captionEditRequest != block_.id()) return;
    m_.captionEditRequest.reset();
    QTimer::singleShot(0, this, [this] { setFocus(); moveCursor(QTextCursor::End); });
}

void CaptionTextField::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        clearFocus();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        cancelled_ = true;
        m_.discardLive();
        settingText_ = true;
        setPlainText(block_.text());
        settingText_ = false;
        clearFocus();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

void CaptionTextField::focusInEvent(QFocusEvent* e) {
    QPlainTextEdit::focusInEvent(e);
    editing_ = block_;
    if (m_.playhead() < block_.start || m_.playhead() >= block_.end) m_.seek((block_.start + block_.end) / 2);
}

void CaptionTextField::focusOutEvent(QFocusEvent* e) {
    QPlainTextEdit::focusOutEvent(e);
    commit();
}

/// Typing ends: what was typed is one undo step. Emptied, the caption's words are left out.
void CaptionTextField::commit() {
    const auto b = editing_;
    const bool cancelled = cancelled_;
    editing_.reset();
    cancelled_ = false;
    if (cancelled || !b) return;
    if (toPlainText().trimmed().isEmpty()) {
        m_.discardLive();
        m_.setCaptionText(*b, QString());
    } else {
        m_.commitLive();
    }
}

// MARK: Rows

QWidget* labelledRow(const RowIcon& icon, const QString& label, std::initializer_list<QWidget*> right, bool dim, int labelWidth) {
    auto* row = new QWidget;
    row->setFixedHeight(28);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto* iv = new RowIconView(icon);
    iv->setColor(dim ? Theme::textFaint() : Theme::textDim());
    h->addWidget(iv);
    auto* l = new QLabel(label);
    l->setFont(Theme::font(11.5));
    l->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg((dim ? Theme::textFaint() : Theme::text()).name()));
    if (labelWidth > 0) l->setFixedWidth(std::max(labelWidth, rowLabelWidth()));
    h->addWidget(l);
    if (labelWidth <= 0) h->addStretch(1);
    for (QWidget* w : right) h->addWidget(w, labelWidth > 0 ? 1 : 0);
    return row;
}

} // namespace er
