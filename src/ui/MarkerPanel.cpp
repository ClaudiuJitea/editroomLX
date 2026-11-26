#include "MarkerPanel.h"
#include "Symbols.h"
#include "Theme.h"
#include "model/Timecode.h"
#include <QAbstractButton>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace er {

namespace {

QString tintName(Marker::Tint t) {
    switch (t) {
    case Marker::Tint::white: return QStringLiteral("White");
    case Marker::Tint::yellow: return QStringLiteral("Yellow");
    case Marker::Tint::green: return QStringLiteral("Green");
    case Marker::Tint::blue: return QStringLiteral("Blue");
    case Marker::Tint::purple: return QStringLiteral("Purple");
    case Marker::Tint::red: return QStringLiteral("Red");
    }
    return {};
}

/// A colour to pick: a 15 px dot, ringed while it's the marker's.
class TintDot : public QAbstractButton {
public:
    TintDot(Marker::Tint t, bool on, QWidget* parent) : QAbstractButton(parent), tint_(t), on_(on) {
        setFixedSize(21, 21);
        setToolTip(tintName(t));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::hex(Marker::hex(tint_)));
        p.drawEllipse(QPointF(10.5, 10.5), 7.5, 7.5);
        if (on_) {
            QColor ring = Theme::text();
            ring.setAlphaF(0.9);
            p.setPen(QPen(ring, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(10.5, 10.5), 9.75, 9.75);
        }
    }
private:
    Marker::Tint tint_;
    bool on_;
};

/// A small icon button (the trash).
class IconKey : public QAbstractButton {
public:
    IconKey(const QString& symbol, const QString& help, QWidget* parent) : QAbstractButton(parent), symbol_(symbol) {
        setFixedSize(22, 22);
        setToolTip(help);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        Symbols::draw(p, symbol_, QPointF(11, 11), 12, underMouse() ? Theme::text() : Theme::textDim(), 500);
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    QString symbol_;
};

/// One marker in the list: its colour, name and time; a click goes to it.
class MarkerRow : public QAbstractButton {
public:
    MarkerRow(const Marker& m, double fps, bool current, QWidget* parent) : QAbstractButton(parent), m_(m), fps_(fps), current_(current) {
        setFixedHeight(26);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || current_) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::hover());
            p.drawRoundedRect(QRectF(rect()), 5, 5);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::hex(Marker::hex(m_.tint)));
        p.drawEllipse(QPointF(6 + 3.5, height() / 2.0), 3.5, 3.5);
        const QString t = Timecode::string(m_.time, fps_);
        const QFont mono = Theme::mono(10.5, 400);
        const double tw = QFontMetricsF(mono).horizontalAdvance(t);
        p.setFont(Theme::font(12));
        p.setPen(current_ ? Theme::text() : Theme::textDim());
        const QRectF nameR(6 + 7 + 9, 0, width() - 22 - tw - 12, height());
        p.drawText(nameR, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(m_.name, Qt::ElideRight, int(nameR.width())));
        p.setFont(mono);
        p.setPen(Theme::textFaint());
        p.drawText(QRectF(width() - 6 - tw, 0, tw, height()), Qt::AlignVCenter | Qt::AlignRight, t);
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    Marker m_;
    double fps_;
    bool current_;
};

} // namespace

MarkerPanel::MarkerPanel(EditorModel& m, QWidget* parent) : QFrame(parent, Qt::Popup), m_(m) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("markerPanel"));
    setStyleSheet(QStringLiteral("#markerPanel { background: %1; border: 1px solid %2; border-radius: 10px; }")
                      .arg(Theme::panel().name(), Theme::stroke().name(QColor::HexArgb)));
    setFixedWidth(270);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(12);
    auto* head = new QHBoxLayout;
    auto* label = new QLabel(QStringLiteral("MARKER"));
    QFont lf = Theme::label();
    lf.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    label->setFont(lf);
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textFaint().name()));
    head->addWidget(label);
    head->addStretch();
    time_ = new QLabel;
    time_->setFont(Theme::mono(10.5, 500));
    time_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textFaint().name()));
    head->addWidget(time_);
    v->addLayout(head);
    name_ = new QLineEdit;
    name_->setPlaceholderText(QStringLiteral("Name"));
    name_->setFont(Theme::font(12.5, 500));
    name_->setFixedHeight(30);
    name_->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 7px; padding: 0 9px; }"
                                        "QLineEdit:focus { border-color: %4; }")
                             .arg(Theme::bg().name(), Theme::text().name(), Theme::stroke().name(QColor::HexArgb), Theme::text().name()));
    connect(name_, &QLineEdit::textEdited, this, [this](const QString& s) {
        if (auto mk = marker(); mk && mk->name != s) m_.renameMarker(id_, s);
    });
    connect(name_, &QLineEdit::returnPressed, this, [this] { m_.setEditingMarker(std::nullopt); });
    v->addWidget(name_);
    tints_ = new QWidget;
    v->addWidget(tints_);
    list_ = new QWidget;
    v->addWidget(list_);
    connect(&m_, &EditorModel::changed, this, &MarkerPanel::refresh);
}

std::optional<Marker> MarkerPanel::marker() const {
    for (const auto& mk : m_.project().markerList()) if (mk.id == id_) return mk;
    return std::nullopt;
}

void MarkerPanel::setMarker(const Uuid& id) {
    const bool changed = id != id_;
    id_ = id;
    if (changed) if (auto mk = marker()) name_->setText(mk->name);
    refresh();
    name_->setFocus();
}

void MarkerPanel::refresh() {
    auto mk = marker();
    if (!mk) { close(); return; }
    time_->setText(Timecode::string(mk->time, m_.project().fps));
    if (!name_->hasFocus() && name_->text() != mk->name) name_->setText(mk->name);
    // The colours and the trash.
    delete tints_->layout();
    for (auto* w : tints_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) w->deleteLater();
    auto* th = new QHBoxLayout(tints_);
    th->setContentsMargins(0, 0, 0, 0);
    th->setSpacing(9);
    for (auto t : {Marker::Tint::white, Marker::Tint::yellow, Marker::Tint::green, Marker::Tint::blue, Marker::Tint::purple, Marker::Tint::red}) {
        auto* dot = new TintDot(t, mk->tint == t, tints_);
        connect(dot, &QAbstractButton::clicked, this, [this, t] { m_.updateMarker(id_, [t](Marker& x) { x.tint = t; }); });
        th->addWidget(dot);
    }
    th->addStretch();
    auto* trash = new IconKey(QStringLiteral("trash"), QStringLiteral("Delete Marker"), tints_);
    connect(trash, &QAbstractButton::clicked, this, [this] { m_.deleteMarker(id_); });
    th->addWidget(trash);
    // Every marker, to jump to.
    delete list_->layout();
    for (auto* w : list_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) w->deleteLater();
    const auto all = m_.project().markerList();
    auto* lv = new QVBoxLayout(list_);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(0);
    list_->setVisible(all.size() > 1);
    if (all.size() > 1) {
        auto* line = new QFrame(list_);
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::stroke().name(QColor::HexArgb)));
        lv->addWidget(line);
        lv->addSpacing(12);
        auto* scroll = new QScrollArea(list_);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(all.size() > 7 ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
        auto* inner = new QWidget;
        auto* iv = new QVBoxLayout(inner);
        iv->setContentsMargins(0, 0, 0, 0);
        iv->setSpacing(2);
        for (const auto& x : all) {
            auto* row = new MarkerRow(x, m_.project().fps, x.id == id_, inner);
            const Uuid xid = x.id;
            const double xt = x.time;
            connect(row, &QAbstractButton::clicked, this, [this, xid, xt] { m_.seek(xt); m_.setEditingMarker(xid); });
            iv->addWidget(row);
        }
        scroll->setWidget(inner);
        scroll->setFixedHeight(int(std::min(all.size() * 28.0, 196.0)));
        lv->addWidget(scroll);
    }
    adjustSize();
}

void MarkerPanel::showAbove(QPoint flag) {
    adjustSize();
    QPoint at(flag.x() - width() / 2, flag.y() - height() - 6);
    if (const QScreen* s = screen()) {
        const QRect g = s->availableGeometry();
        at.setX(std::clamp(at.x(), g.left() + 4, g.right() - width() - 4));
        if (at.y() < g.top()) at.setY(flag.y() + 32);
    }
    move(at);
    show();
    name_->setFocus();
}

void MarkerPanel::closeEvent(QCloseEvent* e) {
    QFrame::closeEvent(e);
}

void MarkerPanel::hideEvent(QHideEvent* e) {
    QFrame::hideEvent(e);
    if (!closing_) {
        closing_ = true;
        emit closed();
        if (!testAttribute(Qt::WA_DeleteOnClose) || isVisible()) return;
        close();
    }
}

} // namespace er
