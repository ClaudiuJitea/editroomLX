#include "TabStrip.h"
#include "Symbols.h"
#include "Theme.h"
#include "Workspace.h"
#include "model/Shortcuts.h"
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <algorithm>
#include <numeric>

namespace er {

namespace {
QFont tabFont() { return Theme::font(12.5, 500); }
QString shortcutText(ShortcutStore& keys, ShortcutAction a, const char* fallback) {
    const auto c = keys.combos(a);
    return c.empty() ? QString::fromUtf8(fallback) : c.front().display();
}
} // namespace

TabStrip::TabStrip(Workspace& w, ShortcutStore& keys, QWidget* parent) : QWidget(parent), w_(w), keys_(keys) {
    setFixedHeight(28);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&w_, &Workspace::tabsChanged, this, qOverload<>(&QWidget::update));
}

/// A tab as wide as it would like: 13 before its name, the dot, room for × (kept on every closable tab, so a tab
/// doesn't change width as it's hovered), and 6 after (13 without ×).
double TabStrip::idealWidth(int i, bool closable) const {
    double w = 13 + QFontMetricsF(tabFont()).horizontalAdvance(w_.title(i));
    if (w_.showsUnsaved(i)) w += 6 + 5;
    if (closable) w += 6 + 16 + 6; else w += 13;
    return std::ceil(w);
}

TabStrip::Layout TabStrip::layout() const {
    Layout L;
    const int n = w_.count();
    const double width = this->width();
    const bool single = n == 1;
    // The tabs that fit, keeping the one in front, and the rest for the menu.
    std::vector<int> shown(static_cast<size_t>(n));
    std::iota(shown.begin(), shown.end(), 0);
    const double per = minTab + spacing;
    double x0 = 0;
    if (double(n) * per + plusWidth > width) {
        const int fit = std::max(1, int((width - plusWidth - moreWidth - 4) / per));
        const int front = w_.activeIndex();
        // The last ones show, unless the one in front is further left: then the run ending at it.
        int from = std::max(0, n - fit);
        if (front < from) from = std::max(0, front - fit + 1);
        shown.clear();
        for (int i = from; i < std::min(n, from + fit); ++i) shown.push_back(i);
        for (int i = 0; i < n; ++i) if (std::find(shown.begin(), shown.end(), i) == shown.end()) L.hidden.push_back(i);
        // "6 more tabs ⌄", first.
        const QString label = QStringLiteral("%1 more tab%2").arg(L.hidden.size()).arg(L.hidden.size() == 1 ? QString() : QStringLiteral("s"));
        const double mw = std::ceil(12 + QFontMetricsF(tabFont()).horizontalAdvance(label) + 5 + 9 + 12);
        L.more = QRectF(0, 0, mw, height());
        x0 = mw + 4;
    }
    // Each as wide as it would like up to a limit, + after them. Short of room, the widest give way first, down to an
    // equal share each.
    std::vector<double> ideal;
    std::vector<bool> closable;
    for (int i : shown) {
        const bool c = !single || !w_.isBlank(i);
        closable.push_back(c);
        ideal.push_back(std::min(maxTab, idealWidth(i, c)));
    }
    std::vector<double> out = ideal;
    double room = width - x0 - plusWidth - spacing * double(shown.size());
    std::vector<size_t> left(shown.size());
    std::iota(left.begin(), left.end(), 0);
    std::stable_sort(left.begin(), left.end(), [&](size_t a, size_t b) { return ideal[a] < ideal[b]; });
    while (!left.empty()) {
        const size_t i = left.front();
        const double share = room / double(left.size());
        if (ideal[i] <= share) {
            room -= ideal[i];
            left.erase(left.begin());
        } else {
            for (size_t j : left) out[j] = std::max(minTab, share);
            break;
        }
    }
    double x = x0;
    for (size_t k = 0; k < shown.size(); ++k) {
        const QRectF box(x, 0, out[k], height());
        const QRectF close(box.right() - 6 - 16, box.center().y() - 8, 16, 16);
        L.items.push_back({shown[k], box, close, closable[k]});
        x += out[k] + spacing;
    }
    L.plus = QRectF(x, (height() - 26) / 2.0, plusWidth, 26);
    return L;
}

std::vector<TabStrip::Item> TabStrip::items() const { return layout().items; }
QRectF TabStrip::plusBox() const { return layout().plus; }
QRectF TabStrip::moreBox() const { return layout().more; }
int TabStrip::hiddenCount() const { return int(layout().hidden.size()); }

void TabStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Layout L = layout();
    const bool single = w_.count() == 1;
    const QFont f = tabFont();
    if (!L.hidden.empty()) {
        const bool hover = L.more.contains(mouse_);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::ink(hover ? 0.08 : 0.04));
        p.drawRoundedRect(L.more, L.more.height() / 2, L.more.height() / 2);
        const QColor col = hover ? Theme::text() : Theme::textDim();
        const QString label = QStringLiteral("%1 more tab%2").arg(L.hidden.size()).arg(L.hidden.size() == 1 ? QString() : QStringLiteral("s"));
        p.setFont(f);
        p.setPen(col);
        const double tw = QFontMetricsF(f).horizontalAdvance(label);
        p.drawText(QRectF(L.more.left() + 12, 0, tw + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, label);
        Symbols::draw(p, QStringLiteral("chevron.down"), QPointF(L.more.left() + 12 + tw + 5 + 4.5, height() / 2.0), 9, col, 600);
    }
    for (const Item& it : L.items) {
        const bool active = it.index == w_.activeIndex();
        const bool hovering = it.box.contains(mouse_);
        const bool showsClose = it.closable && (single ? hovering : active || hovering);
        QColor fill = Qt::transparent;
        if (!single) fill = active ? Theme::ink(0.13) : hovering ? Theme::ink(0.06) : QColor(Qt::transparent);
        if (fill.alpha() > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(it.box, it.box.height() / 2, it.box.height() / 2);
        }
        const bool dot = w_.showsUnsaved(it.index);
        const double right = it.closable ? it.close.left() - 6 : it.box.right() - 13;
        const double textRight = right - (dot ? 6 + 5 : 0);
        p.setFont(f);
        p.setPen(active && !single ? Theme::text() : Theme::textDim());
        const QString name = QFontMetricsF(f).elidedText(w_.title(it.index), Qt::ElideRight, std::max(0.0, textRight - it.box.left() - 13));
        const double nw = QFontMetricsF(f).horizontalAdvance(name);
        p.drawText(QRectF(it.box.left() + 13, 0, nw + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, name);
        if (dot) {
            p.setPen(Qt::NoPen);
            p.setBrush(active ? Theme::textDim() : Theme::textFaint());
            p.drawEllipse(QPointF(it.box.left() + 13 + nw + 6 + 2.5, height() / 2.0), 2.5, 2.5);
        }
        if (showsClose) {
            const bool overClose = it.close.contains(mouse_);
            if (overClose) {
                p.setPen(Qt::NoPen);
                p.setBrush(Theme::ink(0.12));
                p.drawEllipse(it.close);
            }
            Symbols::draw(p, QStringLiteral("xmark"), it.close.center(), 8, overClose ? Theme::text() : Theme::textDim(), 700);
        }
    }
    const bool plusHover = L.plus.contains(mouse_);
    if (plusHover) {
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::ink(0.08));
        p.drawEllipse(L.plus);
    }
    Symbols::draw(p, QStringLiteral("plus"), L.plus.center(), 11.5, plusHover ? Theme::text() : Theme::textDim(), 500);
}

void TabStrip::mouseMoveEvent(QMouseEvent* e) {
    mouse_ = e->position();
    update();
}

void TabStrip::leaveEvent(QEvent*) {
    mouse_ = QPointF(-1, -1);
    update();
}

void TabStrip::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const Layout L = layout();
    const QPointF at = e->position();
    if (L.plus.contains(at)) { w_.newTab(); return; }
    if (!L.hidden.empty() && L.more.contains(at)) {
        // The tabs that don't fit, to pick from.
        QMenu menu(this);
        for (int i : L.hidden) menu.addAction(w_.title(i) + (w_.showsUnsaved(i) ? QStringLiteral(" •") : QString()), this, [this, i] { w_.select(i); });
        menu.exec(mapToGlobal(QPoint(int(L.more.left()), int(L.more.bottom()) + 2)));
        return;
    }
    const bool single = w_.count() == 1;
    for (const Item& it : L.items) {
        if (!it.box.contains(at)) continue;
        const bool active = it.index == w_.activeIndex();
        const bool showsClose = it.closable && (single ? true : active || it.box.contains(at));
        if (showsClose && it.close.contains(at)) { pressedClose_ = true; return; }
        w_.select(it.index);
        return;
    }
}

void TabStrip::mouseReleaseEvent(QMouseEvent* e) {
    if (!pressedClose_) return;
    pressedClose_ = false;
    for (const Item& it : layout().items)
        if (it.close.contains(e->position()) && it.box.contains(e->position())) { w_.close(it.index); return; }
}

void TabStrip::contextMenuEvent(QContextMenuEvent* e) {
    for (const Item& it : layout().items) {
        if (!it.box.contains(e->pos())) continue;
        const int i = it.index;
        const QString s = w_.slot(i);
        QMenu menu(this);
        // By slot: closing one tab moves the others along.
        auto find = [this, s] { for (int k = 0; k < w_.count(); ++k) if (w_.slot(k) == s) return k; return -1; };
        menu.addAction(QStringLiteral("Close Tab"), this, [this, find] { w_.close(find()); });
        if (w_.count() > 1) menu.addAction(QStringLiteral("Close Other Tabs"), this, [this, find] { w_.closeOthers(find()); });
        menu.exec(e->globalPos());
        return;
    }
}

bool TabStrip::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* h = static_cast<QHelpEvent*>(e);
        const Layout L = layout();
        QString tip;
        if (L.plus.contains(h->pos())) tip = QStringLiteral("New Tab (%1)").arg(shortcutText(keys_, ShortcutAction::newTab, "Ctrl+T"));
        for (const Item& it : L.items) {
            if (!it.box.contains(h->pos())) continue;
            if (it.closable && it.close.contains(h->pos())) tip = QStringLiteral("Close Tab (%1)").arg(shortcutText(keys_, ShortcutAction::closeTab, "Ctrl+W"));
            else tip = w_.openPath(it.index).isEmpty() ? w_.title(it.index) : w_.openPath(it.index);
        }
        if (tip.isEmpty()) QToolTip::hideText(); else QToolTip::showText(h->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(e);
}

} // namespace er
