// The small icons the Mac draws with SF Symbols, drawn here as line icons (SF Symbols may only be used on
// Apple platforms). Each is named for the symbol it stands in for ("sun.max", "drop", …) so the ported
// views read like the Mac's. Drawn in the pen colour into a square of `size` px around `centre`.
#pragma once
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QString>

namespace er::Symbols {
/// Draws `name` centred on `centre`, about `size` px tall (the point size the Mac gives the symbol).
/// `weight` is 400 (regular) to 700 (bold). Unknown names draw nothing.
void draw(QPainter& p, const QString& name, QPointF centre, double size, const QColor& color, int weight = 500);
/// As an icon, for buttons and menus.
QIcon icon(const QString& name, double size, const QColor& color, int weight = 500);
bool has(const QString& name);
} // namespace er::Symbols
