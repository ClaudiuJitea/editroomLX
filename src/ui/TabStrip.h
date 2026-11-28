// TabStrip.swift: the open projects, as tabs across the title strip, each as wide as its name (up to a limit), with +
// for a new one. Only the tab in front has a pill; the others are just their names until hovered. With more than fit,
// they shrink evenly and their names are cut short, and the rest go in a "6 more tabs ⌄" menu. One project shows just
// its name (with × on hover to close it, unless it's already empty).
#pragma once
#include <QWidget>

namespace er {

class Workspace;
class ShortcutStore;

class TabStrip : public QWidget {
    Q_OBJECT
public:
    TabStrip(Workspace& w, ShortcutStore& keys, QWidget* parent = nullptr);
    /// Narrower than this, tabs are put in the "more tabs" menu instead.
    static constexpr double minTab = 84, maxTab = 190, spacing = 2, plusWidth = 26, moreWidth = 124;
    QSize sizeHint() const override { return {600, 28}; }
    QSize minimumSizeHint() const override { return {160, 28}; }

    /// Where each shown tab is (for tests): its index in the workspace and its box.
    struct Item { int index; QRectF box; QRectF close; bool closable; };
    std::vector<Item> items() const;
    QRectF plusBox() const;
    QRectF moreBox() const;
    int hiddenCount() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    bool event(QEvent*) override;

private:
    struct Layout { std::vector<Item> items; QRectF plus, more; std::vector<int> hidden; };
    Layout layout() const;
    double idealWidth(int i, bool closable) const;
    Workspace& w_;
    ShortcutStore& keys_;
    QPointF mouse_{-1, -1};
    bool pressedClose_ = false;
};

} // namespace er
