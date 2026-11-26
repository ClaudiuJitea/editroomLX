// Small shared widgets: pill buttons, toggle chips, hairlines, section headers.
#pragma once
#include <QLabel>
#include <QPushButton>
#include <QWidget>

namespace er {

/// The Mac's PillButton: rounded, quiet or prominent (white with dark type).
class PillButton : public QPushButton {
    Q_OBJECT
public:
    PillButton(const QString& title, bool prominent = false, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { hover_ = true; update(); }
    void leaveEvent(QEvent*) override { hover_ = false; update(); }
private:
    bool prominent_;
    bool hover_ = false;
};

/// A text toggle in a toolbar ("Snap", "Sticky"…): dim when off, bright when on.
class ToggleChip : public QPushButton {
    Q_OBJECT
public:
    explicit ToggleChip(const QString& title, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
};

/// A one-pixel line in the stroke colour.
class Hairline : public QWidget {
public:
    explicit Hairline(Qt::Orientation o, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
};

/// "VIDEO FADE": small caps section label.
QLabel* sectionLabel(const QString& text);

} // namespace er
