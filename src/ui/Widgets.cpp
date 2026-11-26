#include "Widgets.h"
#include "Theme.h"
#include <QPainter>

namespace er {

PillButton::PillButton(const QString& title, bool prominent, QWidget* parent) : QPushButton(title, parent), prominent_(prominent) {
    setCursor(Qt::PointingHandCursor);
    setFont(Theme::font(12, 600));
    setFixedHeight(26);
    setMinimumWidth(QFontMetrics(font()).horizontalAdvance(title) + 28);
}

void PillButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg = prominent_ ? Theme::text() : (hover_ ? Theme::buttonHover() : Theme::button());
    if (prominent_ && hover_) bg = bg.darker(108);
    if (!isEnabled()) bg.setAlphaF(0.5f);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), height() / 2.0, height() / 2.0);
    p.setPen(prominent_ ? Theme::onText() : Theme::text());
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

ToggleChip::ToggleChip(const QString& title, QWidget* parent) : QPushButton(title, parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFont(Theme::font(11.5, 500));
    setFixedHeight(22);
    setMinimumWidth(QFontMetrics(font()).horizontalAdvance(title) + 16);
}

void ToggleChip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (underMouse()) {
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::hover());
        p.drawRoundedRect(rect(), 5, 5);
    }
    p.setPen(isChecked() ? Theme::text() : Theme::textFaint());
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

Hairline::Hairline(Qt::Orientation o, QWidget* parent) : QWidget(parent) {
    if (o == Qt::Horizontal) setFixedHeight(1); else setFixedWidth(1);
}

void Hairline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::bg());
    p.fillRect(rect(), Theme::stroke());
}

QLabel* sectionLabel(const QString& text) {
    auto* l = new QLabel(text.toUpper());
    l->setFont(Theme::label(10));
    l->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textFaint().name()));
    return l;
}

} // namespace er
