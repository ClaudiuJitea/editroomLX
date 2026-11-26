#include "Symbols.h"
#include <QPainterPath>
#include <QPixmap>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <QTransform>

namespace er::Symbols {

namespace {

/// Drawn in a unit box: x and y from -1 to 1, y down. `P` scales it to the size asked for.
struct Ctx {
    QPainter& p;
    QPointF c;
    double s; // half the size
    QPointF at(double x, double y) const { return {c.x() + x * s, c.y() + y * s}; }
    double len(double v) const { return v * s; }
    void line(double x0, double y0, double x1, double y1) const { p.drawLine(at(x0, y0), at(x1, y1)); }
    void circle(double x, double y, double r) const { p.drawEllipse(at(x, y), len(r), len(r)); }
    void rect(double x0, double y0, double x1, double y1, double radius = 0) const {
        p.drawRoundedRect(QRectF(at(x0, y0), at(x1, y1)), len(radius), len(radius));
    }
    void arc(double x, double y, double r, double fromDeg, double spanDeg) const {
        p.drawArc(QRectF(at(x - r, y - r), at(x + r, y + r)), int(fromDeg * 16), int(spanDeg * 16));
    }
    void poly(std::initializer_list<QPointF> pts, bool closed = false) const {
        QPainterPath path;
        bool first = true;
        for (const QPointF& q : pts) {
            if (first) path.moveTo(at(q.x(), q.y())); else path.lineTo(at(q.x(), q.y()));
            first = false;
        }
        if (closed) path.closeSubpath();
        p.drawPath(path);
    }
    void fill(const QPainterPath& path) const { p.save(); p.setBrush(p.pen().color()); p.setPen(Qt::NoPen); p.drawPath(path); p.restore(); }
    /// An arrow head at (x, y) pointing in direction (dx, dy).
    void head(double x, double y, double dx, double dy, double size = 0.32) const {
        const double l = std::hypot(dx, dy);
        dx /= l; dy /= l;
        const double nx = -dy, ny = dx;
        poly({{x - dx * size + nx * size, y - dy * size + ny * size}, {x, y}, {x - dx * size - nx * size, y - dy * size - ny * size}});
    }
};

using Draw = std::function<void(const Ctx&)>;

const std::map<QString, Draw>& table() {
    static const std::map<QString, Draw> t = {
        {"sun.max", [](const Ctx& k) {
            k.circle(0, 0, 0.36);
            for (int i = 0; i < 8; ++i) {
                const double a = i * M_PI / 4;
                k.line(std::cos(a) * 0.6, std::sin(a) * 0.6, std::cos(a) * 0.85, std::sin(a) * 0.85);
            }
        }},
        {"circle.lefthalf.filled", [](const Ctx& k) {
            k.circle(0, 0, 0.8);
            QPainterPath h;
            h.moveTo(k.at(0, -0.8));
            h.arcTo(QRectF(k.at(-0.8, -0.8), k.at(0.8, 0.8)), 90, 180);
            h.closeSubpath();
            k.fill(h);
        }},
        {"drop", [](const Ctx& k) {
            QPainterPath d;
            d.moveTo(k.at(0, -0.9));
            d.cubicTo(k.at(0.2, -0.45), k.at(0.65, -0.1), k.at(0.65, 0.3));
            d.cubicTo(k.at(0.65, 0.7), k.at(0.3, 0.9), k.at(0, 0.9));
            d.cubicTo(k.at(-0.3, 0.9), k.at(-0.65, 0.7), k.at(-0.65, 0.3));
            d.cubicTo(k.at(-0.65, -0.1), k.at(-0.2, -0.45), k.at(0, -0.9));
            k.p.drawPath(d);
        }},
        {"thermometer.medium", [](const Ctx& k) {
            k.rect(-0.2, -0.9, 0.2, 0.35, 0.2);
            k.circle(0, 0.55, 0.34);
            k.line(0, 0.55, 0, -0.15);
        }},
        {"eyedropper", [](const Ctx& k) {
            k.line(-0.75, 0.75, 0.25, -0.25);
            k.line(-0.75, 0.75, -0.6, 0.45);
            k.line(0.05, -0.45, 0.45, -0.05);
            k.circle(0.45, -0.45, 0.28);
        }},
        {"camera.filters", [](const Ctx& k) {
            k.circle(-0.28, -0.2, 0.5);
            k.circle(0.28, -0.2, 0.5);
            k.circle(0, 0.3, 0.5);
        }},
        {"circle.dotted", [](const Ctx& k) {
            QPen pen = k.p.pen();
            pen.setStyle(Qt::DotLine);
            k.p.save();
            k.p.setPen(pen);
            k.circle(0, 0, 0.8);
            k.p.restore();
        }},
        {"speaker.wave.2", [](const Ctx& k) {
            k.poly({{-0.9, -0.25}, {-0.55, -0.25}, {-0.15, -0.65}, {-0.15, 0.65}, {-0.55, 0.25}, {-0.9, 0.25}}, true);
            k.arc(-0.15, 0, 0.45, -45, 90);
            k.arc(-0.15, 0, 0.8, -45, 90);
        }},
        {"clock.arrow.circlepath", [](const Ctx& k) {
            k.arc(0, 0, 0.8, 120, 300);
            k.head(-0.4, -0.69, -0.6, 0.35, 0.28);
            k.line(0, 0, 0, -0.42);
            k.line(0, 0, 0.32, 0.2);
        }},
        {"arrow.up.left.and.arrow.down.right", [](const Ctx& k) {
            k.line(-0.8, -0.8, -0.15, -0.15);
            k.poly({{-0.8, -0.3}, {-0.8, -0.8}, {-0.3, -0.8}});
            k.line(0.8, 0.8, 0.15, 0.15);
            k.poly({{0.8, 0.3}, {0.8, 0.8}, {0.3, 0.8}});
        }},
        {"arrow.left.and.right", [](const Ctx& k) {
            k.line(-0.85, 0, 0.85, 0);
            k.head(-0.85, 0, -1, 0);
            k.head(0.85, 0, 1, 0);
        }},
        {"arrow.up.and.down", [](const Ctx& k) {
            k.line(0, -0.85, 0, 0.85);
            k.head(0, -0.85, 0, -1);
            k.head(0, 0.85, 0, 1);
        }},
        {"arrow.right", [](const Ctx& k) {
            k.line(-0.8, 0, 0.8, 0);
            k.head(0.8, 0, 1, 0, 0.4);
        }},
        {"timer", [](const Ctx& k) {
            k.circle(0, 0.1, 0.75);
            k.line(0, 0.1, 0.3, -0.25);
            k.line(-0.2, -0.85, 0.2, -0.85);
        }},
        {"arrow.right.to.line", [](const Ctx& k) {
            k.line(-0.85, 0, 0.45, 0);
            k.head(0.45, 0, 1, 0, 0.35);
            k.line(0.8, -0.7, 0.8, 0.7);
        }},
        {"arrow.right.to.line.compact", [](const Ctx& k) {
            k.line(-0.6, 0, 0.3, 0);
            k.head(0.3, 0, 1, 0, 0.3);
            k.line(0.6, -0.5, 0.6, 0.5);
        }},
        {"textformat.size", [](const Ctx& k) {
            k.poly({{-0.95, 0.75}, {-0.6, -0.15}, {-0.25, 0.75}});
            k.line(-0.8, 0.4, -0.4, 0.4);
            k.poly({{-0.05, 0.75}, {0.45, -0.75}, {0.95, 0.75}});
            k.line(0.15, 0.25, 0.75, 0.25);
        }},
        {"textformat", [](const Ctx& k) {
            k.poly({{-0.95, 0.75}, {-0.5, -0.6}, {-0.05, 0.75}});
            k.line(-0.75, 0.25, -0.25, 0.25);
            k.circle(0.5, 0.42, 0.33);
            k.line(0.83, 0.05, 0.83, 0.75);
        }},
        {"highlighter", [](const Ctx& k) {
            k.poly({{-0.35, 0.45}, {0.45, -0.75}, {0.85, -0.45}, {0.05, 0.75}}, true);
            k.poly({{-0.35, 0.45}, {-0.75, 0.85}, {-0.2, 0.85}, {0.05, 0.75}});
        }},
        {"a.square", [](const Ctx& k) {
            k.rect(-0.85, -0.85, 0.85, 0.85, 0.25);
            k.poly({{-0.45, 0.5}, {0, -0.5}, {0.45, 0.5}});
            k.line(-0.27, 0.15, 0.27, 0.15);
        }},
        {"paintbrush", [](const Ctx& k) {
            k.line(0.8, -0.8, -0.1, 0.2);
            QPainterPath b;
            b.moveTo(k.at(-0.1, 0.1));
            b.cubicTo(k.at(-0.5, 0.1), k.at(-0.6, 0.5), k.at(-0.85, 0.85));
            b.cubicTo(k.at(-0.4, 0.8), k.at(0.05, 0.6), k.at(0.05, 0.3));
            k.p.drawPath(b);
        }},
        {"chevron.right", [](const Ctx& k) { k.poly({{-0.3, -0.75}, {0.4, 0}, {-0.3, 0.75}}); }},
        {"chevron.down", [](const Ctx& k) { k.poly({{-0.75, -0.3}, {0, 0.4}, {0.75, -0.3}}); }},
        {"magnifyingglass", [](const Ctx& k) {
            k.circle(-0.15, -0.15, 0.58);
            k.line(0.28, 0.28, 0.85, 0.85);
        }},
        {"checkmark", [](const Ctx& k) { k.poly({{-0.75, 0.05}, {-0.25, 0.6}, {0.8, -0.6}}); }},
        {"eye", [](const Ctx& k) {
            QPainterPath e;
            e.moveTo(k.at(-0.95, 0));
            e.quadTo(k.at(0, -0.95), k.at(0.95, 0));
            e.quadTo(k.at(0, 0.95), k.at(-0.95, 0));
            k.p.drawPath(e);
            k.circle(0, 0, 0.27);
        }},
        {"eye.slash", [](const Ctx& k) {
            QPainterPath e;
            e.moveTo(k.at(-0.95, 0));
            e.quadTo(k.at(0, -0.95), k.at(0.95, 0));
            e.quadTo(k.at(0, 0.95), k.at(-0.95, 0));
            k.p.drawPath(e);
            k.circle(0, 0, 0.27);
            k.line(-0.8, -0.8, 0.8, 0.8);
        }},
        {"film", [](const Ctx& k) {
            k.rect(-0.7, -0.85, 0.7, 0.85, 0.12);
            k.line(-0.35, -0.85, -0.35, 0.85);
            k.line(0.35, -0.85, 0.35, 0.85);
            for (double y : {-0.45, 0.0, 0.45}) { k.line(-0.7, y, -0.35, y); k.line(0.35, y, 0.7, y); }
        }},
        {"waveform", [](const Ctx& k) {
            const double hs[] = {0.2, 0.5, 0.85, 0.45, 0.7, 0.3, 0.15};
            for (int i = 0; i < 7; ++i) {
                const double x = -0.84 + i * 0.28;
                k.line(x, -hs[i], x, hs[i]);
            }
        }},
        {"photo", [](const Ctx& k) {
            k.rect(-0.9, -0.7, 0.9, 0.7, 0.15);
            k.poly({{-0.9, 0.5}, {-0.35, -0.05}, {0.1, 0.4}, {0.4, 0.15}, {0.9, 0.6}});
            k.circle(0.4, -0.3, 0.13);
        }},
        {"square.fill", [](const Ctx& k) {
            QPainterPath r;
            r.addRoundedRect(QRectF(k.at(-0.8, -0.8), k.at(0.8, 0.8)), k.len(0.2), k.len(0.2));
            k.fill(r);
        }},
        {"square.stack", [](const Ctx& k) {
            k.rect(-0.75, -0.35, 0.55, 0.85, 0.15);
            k.poly({{-0.5, -0.6}, {0.8, -0.6}, {0.8, 0.6}});
        }},
        {"captions.bubble", [](const Ctx& k) {
            QPainterPath b;
            b.addRoundedRect(QRectF(k.at(-0.9, -0.75), k.at(0.9, 0.5)), k.len(0.25), k.len(0.25));
            k.p.drawPath(b);
            k.poly({{-0.35, 0.5}, {-0.5, 0.9}, {0, 0.5}});
            k.line(-0.55, -0.25, 0.1, -0.25);
            k.line(-0.55, 0.05, 0.55, 0.05);
        }},
        {"rectangle.on.rectangle", [](const Ctx& k) {
            k.rect(-0.9, -0.35, 0.45, 0.65, 0.12);
            k.poly({{-0.55, -0.65}, {0.9, -0.65}, {0.9, 0.35}});
        }},
        {"scissors", [](const Ctx& k) {
            k.circle(-0.5, 0.55, 0.28);
            k.circle(0.5, 0.55, 0.28);
            k.line(-0.32, 0.33, 0.45, -0.85);
            k.line(0.32, 0.33, -0.45, -0.85);
        }},
        {"trash", [](const Ctx& k) {
            k.line(-0.8, -0.55, 0.8, -0.55);
            k.poly({{-0.3, -0.55}, {-0.25, -0.85}, {0.25, -0.85}, {0.3, -0.55}});
            k.poly({{-0.6, -0.55}, {-0.5, 0.85}, {0.5, 0.85}, {0.6, -0.55}});
            k.line(-0.2, -0.25, -0.17, 0.55);
            k.line(0.2, -0.25, 0.17, 0.55);
        }},
        {"cursorarrow", [](const Ctx& k) {
            QPainterPath a;
            a.moveTo(k.at(-0.55, -0.85));
            a.lineTo(k.at(0.6, 0.25));
            a.lineTo(k.at(0.05, 0.3));
            a.lineTo(k.at(0.35, 0.85));
            a.lineTo(k.at(0.12, 0.95));
            a.lineTo(k.at(-0.15, 0.4));
            a.lineTo(k.at(-0.55, 0.75));
            a.closeSubpath();
            k.fill(a);
        }},
        {"lock", [](const Ctx& k) {
            QPainterPath body;
            body.addRoundedRect(QRectF(k.at(-0.7, -0.1), k.at(0.7, 0.9)), k.len(0.15), k.len(0.15));
            k.fill(body);
            k.arc(0, -0.25, 0.42, 0, 180);
            k.line(-0.42, -0.25, -0.42, -0.1);
            k.line(0.42, -0.25, 0.42, -0.1);
        }},
        {"arrow.down", [](const Ctx& k) {
            k.line(0, -0.85, 0, 0.8);
            k.head(0, 0.85, 0, 1, 0.45);
        }},
        {"gearshape", [](const Ctx& k) {
            QPainterPath g;
            for (int i = 0; i < 16; ++i) {
                const double a = i * M_PI / 8;
                const double r = (i % 2 == 0) ? 0.88 : 0.66;
                const QPointF pt = k.at(std::cos(a) * r, std::sin(a) * r);
                if (i == 0) g.moveTo(pt); else g.lineTo(pt);
            }
            g.closeSubpath();
            k.p.drawPath(g);
            k.circle(0, 0, 0.28);
        }},
        {"keyboard", [](const Ctx& k) {
            k.rect(-0.95, -0.55, 0.95, 0.55, 0.15);
            for (double y : {-0.22, 0.05}) for (double x = -0.65; x <= 0.66; x += 0.26) k.line(x, y, x + 0.02, y);
            k.line(-0.4, 0.3, 0.4, 0.3);
        }},
        {"wand.and.sparkles", [](const Ctx& k) {
            k.line(-0.85, 0.85, 0.25, -0.25);
            k.line(0.6, -0.95, 0.6, -0.45); k.line(0.35, -0.7, 0.85, -0.7);
            k.line(-0.35, -0.75, -0.35, -0.45); k.line(-0.5, -0.6, -0.2, -0.6);
            k.line(0.75, 0.1, 0.75, 0.4); k.line(0.6, 0.25, 0.9, 0.25);
        }},
        {"arrow.down.circle", [](const Ctx& k) {
            k.circle(0, 0, 0.85);
            k.line(0, -0.45, 0, 0.42);
            k.poly({{-0.32, 0.12}, {0, 0.45}, {0.32, 0.12}});
        }},
        {"play.fill", [](const Ctx& k) {
            QPainterPath t;
            t.moveTo(k.at(-0.6, -0.8)); t.lineTo(k.at(0.8, 0)); t.lineTo(k.at(-0.6, 0.8)); t.closeSubpath();
            k.p.save(); k.p.setPen(QPen(k.p.pen().color(), k.len(0.2), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); k.p.setBrush(k.p.pen().color());
            k.p.drawPath(t); k.p.restore();
        }},
        {"pause.fill", [](const Ctx& k) {
            QPainterPath b;
            b.addRoundedRect(QRectF(k.at(-0.65, -0.8), k.at(-0.15, 0.8)), k.len(0.1), k.len(0.1));
            b.addRoundedRect(QRectF(k.at(0.15, -0.8), k.at(0.65, 0.8)), k.len(0.1), k.len(0.1));
            k.fill(b);
        }},
        {"backward.end.fill", [](const Ctx& k) {
            QPainterPath t;
            t.addRect(QRectF(k.at(-0.75, -0.65), k.at(-0.55, 0.65)));
            QPainterPath tri;
            tri.moveTo(k.at(0.7, -0.65)); tri.lineTo(k.at(-0.45, 0)); tri.lineTo(k.at(0.7, 0.65)); tri.closeSubpath();
            k.fill(t.united(tri));
        }},
        {"forward.end.fill", [](const Ctx& k) {
            QPainterPath t;
            t.addRect(QRectF(k.at(0.55, -0.65), k.at(0.75, 0.65)));
            QPainterPath tri;
            tri.moveTo(k.at(-0.7, -0.65)); tri.lineTo(k.at(0.45, 0)); tri.lineTo(k.at(-0.7, 0.65)); tri.closeSubpath();
            k.fill(t.united(tri));
        }},
        {"backward.frame.fill", [](const Ctx& k) {
            QPainterPath tri;
            tri.moveTo(k.at(0.35, -0.65)); tri.lineTo(k.at(-0.6, 0)); tri.lineTo(k.at(0.35, 0.65)); tri.closeSubpath();
            QPainterPath bar;
            bar.addRect(QRectF(k.at(0.5, -0.65), k.at(0.7, 0.65)));
            k.fill(tri.united(bar));
        }},
        {"forward.frame.fill", [](const Ctx& k) {
            QPainterPath tri;
            tri.moveTo(k.at(-0.35, -0.65)); tri.lineTo(k.at(0.6, 0)); tri.lineTo(k.at(-0.35, 0.65)); tri.closeSubpath();
            QPainterPath bar;
            bar.addRect(QRectF(k.at(-0.7, -0.65), k.at(-0.5, 0.65)));
            k.fill(tri.united(bar));
        }},
        {"chevron.left", [](const Ctx& k) { k.poly({{0.3, -0.75}, {-0.4, 0}, {0.3, 0.75}}); }},
        {"exclamationmark.triangle.fill", [](const Ctx& k) {
            QPainterPath t;
            t.moveTo(k.at(0, -0.9));
            t.lineTo(k.at(0.95, 0.8));
            t.lineTo(k.at(-0.95, 0.8));
            t.closeSubpath();
            k.fill(t);
            k.p.save();
            k.p.setPen(QPen(Qt::black, k.len(0.16), Qt::SolidLine, Qt::RoundCap));
            k.line(0, -0.3, 0, 0.22);
            k.line(0, 0.52, 0, 0.53);
            k.p.restore();
        }},
        {"text.bubble", [](const Ctx& k) {
            QPainterPath b;
            b.addRoundedRect(QRectF(k.at(-0.9, -0.8), k.at(0.9, 0.45)), k.len(0.3), k.len(0.3));
            k.p.drawPath(b);
            k.poly({{-0.4, 0.45}, {-0.55, 0.9}, {0.05, 0.45}});
            k.line(-0.5, -0.35, 0.5, -0.35);
            k.line(-0.5, 0, 0.25, 0);
        }},
        {"xmark.circle.fill", [](const Ctx& k) {
            // A filled circle with the cross cut out of it.
            QPainterPath c;
            c.addEllipse(k.at(0, 0), k.len(0.9), k.len(0.9));
            QPainterPath x;
            x.moveTo(k.at(-0.35, -0.35)); x.lineTo(k.at(0.35, 0.35));
            x.moveTo(k.at(0.35, -0.35)); x.lineTo(k.at(-0.35, 0.35));
            QPainterPathStroker st;
            st.setWidth(k.len(0.2));
            st.setCapStyle(Qt::RoundCap);
            k.fill(c.subtracted(st.createStroke(x)));
        }},
        {"folder", [](const Ctx& k) {
            k.poly({{-0.9, 0.7}, {-0.9, -0.7}, {-0.3, -0.7}, {-0.15, -0.45}, {0.9, -0.45}, {0.9, 0.7}}, true);
            k.line(-0.9, -0.2, 0.9, -0.2);
        }},
        {"textframe", [](const Ctx& k) {
            // TextFrameIcon: a rounded frame with a T, on the frame's 17 × 13 grid (drawn 1.15 × 0.86 of the size).
            const double w = 1.15, h = 0.86;
            auto at = [&](double gx, double gy) { return k.at(-w + gx / 17 * 2 * w, -h + gy / 13 * 2 * h); };
            k.p.drawRoundedRect(QRectF(at(0.4, 0.4), at(16.6, 12.6)), k.len(0.25), k.len(0.25));
            k.p.drawLine(at(5, 4), at(12, 4));
            k.p.drawLine(at(8.5, 4), at(8.5, 9.5));
        }},
        {"solid", [](const Ctx& k) {
            // SolidIcon: a 16:9 frame filled with colour.
            const double w = 1.15, h = 0.86;
            k.p.drawRoundedRect(QRectF(k.at(-w + 0.06, -h + 0.06), k.at(w - 0.06, h - 0.06)), k.len(0.25), k.len(0.25));
            QPainterPath in;
            in.addRoundedRect(QRectF(k.at(-w + 0.4, -h + 0.4), k.at(w - 0.4, h - 0.4)), k.len(0.1), k.len(0.1));
            k.fill(in);
        }},
        {"xmark", [](const Ctx& k) { k.line(-0.6, -0.6, 0.6, 0.6); k.line(0.6, -0.6, -0.6, 0.6); }},
        {"plus", [](const Ctx& k) { k.line(-0.7, 0, 0.7, 0); k.line(0, -0.7, 0, 0.7); }},
        {"folder.badge.plus", [](const Ctx& k) {
            k.poly({{-0.9, 0.7}, {-0.9, -0.7}, {-0.3, -0.7}, {-0.15, -0.45}, {0.5, -0.45}}, false);
            k.poly({{-0.9, 0.7}, {0.2, 0.7}});
            k.line(0.6, 0.0, 0.6, 0.9);
            k.line(0.15, 0.45, 1.05, 0.45);
        }},
    };
    return t;
}

/// LineIcon (Theme.swift): the Mac's own line drawings on a 16 × 16 grid, used in place of SF Symbols for these
/// names on the Mac too, so they're ported point for point.
const std::set<QString>& lineIconNames() {
    static const std::set<QString> n = {"cursorarrow", "scissors", "square.and.arrow.down", "eye", "eye.slash", "lock", "repeat",
                                        "arrow.left.and.right", "arrow.up.left.and.arrow.down.right", "arrow.down.right.and.arrow.up.left", "trash"};
    return n;
}

QPainterPath lineIcon(const QString& name) {
    QPainterPath p;
    auto line = [&p](std::initializer_list<QPointF> pts) {
        bool first = true;
        for (const QPointF& q : pts) { if (first) p.moveTo(q); else p.lineTo(q); first = false; }
    };
    if (name == "cursorarrow") {
        line({{3, 2}, {13, 8.2}, {8.6, 9.1}, {11.1, 13.5}, {9.5, 14.4}, {7, 10}, {3.6, 12}});
        p.closeSubpath();
    } else if (name == "scissors") {
        p.addEllipse(QRectF(1.7, 9.7, 4.6, 4.6));
        p.addEllipse(QRectF(9.7, 9.7, 4.6, 4.6));
        line({{5.6, 10.4}, {13, 2}});
        line({{10.4, 10.4}, {3, 2}});
    } else if (name == "square.and.arrow.down") {
        line({{8, 1.5}, {8, 10}});
        line({{5, 7}, {8, 10}, {11, 7}});
        p.moveTo(3, 9.5); p.lineTo(3, 12.5);
        p.quadTo(3, 14, 4.5, 14);
        p.lineTo(11.5, 14);
        p.quadTo(13, 14, 13, 12.5);
        p.lineTo(13, 9.5);
    } else if (name == "eye" || name == "eye.slash") {
        p.moveTo(1.5, 8);
        p.quadTo(8, 1, 14.5, 8);
        p.quadTo(8, 15, 1.5, 8);
        p.addEllipse(QRectF(6, 6, 4, 4));
        if (name == "eye.slash") line({{2.5, 13.5}, {13.5, 2.5}});
    } else if (name == "lock") {
        p.addRoundedRect(QRectF(3, 7, 10, 7.5), 1.5, 1.5);
        p.moveTo(5.2, 7); p.lineTo(5.2, 5);
        p.arcTo(QRectF(8 - 2.8, 5 - 2.8, 5.6, 5.6), 180, -180);
        p.lineTo(10.8, 7);
    } else if (name == "repeat") {
        p.moveTo(2, 7); p.lineTo(2, 6);
        p.quadTo(2, 3.5, 4.5, 3.5);
        p.lineTo(13, 3.5);
        line({{11, 1.5}, {13, 3.5}, {11, 5.5}});
        p.moveTo(14, 9); p.lineTo(14, 10);
        p.quadTo(14, 12.5, 11.5, 12.5);
        p.lineTo(3, 12.5);
        line({{5, 14.5}, {3, 12.5}, {5, 10.5}});
    } else if (name == "arrow.left.and.right") {
        line({{1.5, 8}, {14.5, 8}});
        line({{4.5, 5}, {1.5, 8}, {4.5, 11}});
        line({{11.5, 5}, {14.5, 8}, {11.5, 11}});
    } else if (name == "arrow.up.left.and.arrow.down.right") {
        line({{2, 6}, {2, 2}, {6, 2}}); line({{2, 2}, {6.5, 6.5}});
        line({{14, 10}, {14, 14}, {10, 14}}); line({{14, 14}, {9.5, 9.5}});
    } else if (name == "arrow.down.right.and.arrow.up.left") {
        line({{6.5, 2.5}, {6.5, 6.5}, {2.5, 6.5}}); line({{2, 2}, {6.5, 6.5}});
        line({{9.5, 13.5}, {9.5, 9.5}, {13.5, 9.5}}); line({{14, 14}, {9.5, 9.5}});
    } else if (name == "trash") {
        line({{2.5, 4.5}, {13.5, 4.5}});
        line({{6, 4.5}, {6, 3}, {10, 3}, {10, 4.5}});
        line({{4, 4.5}, {4.7, 13.5}, {11.3, 13.5}, {12, 4.5}});
    }
    return p;
}

} // namespace

bool has(const QString& name) { return table().count(name) > 0 || lineIconNames().count(name) > 0; }

void draw(QPainter& p, const QString& name, QPointF centre, double size, const QColor& color, int weight) {
    if (lineIconNames().count(name)) {
        // Glyph: a line icon in a square 1.2 times the symbol size, stroked at 1.3.
        const double side = size * 1.2;
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(centre.x() - side / 2, centre.y() - side / 2);
        const QPainterPath path = QTransform::fromScale(side / 16, side / 16).map(lineIcon(name));
        p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.restore();
        return;
    }
    const auto it = table().find(name);
    if (it == table().end()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // Line weight by the symbol's point size and weight, as SF Symbols thicken.
    const double w = std::max(1.0, size * (weight >= 700 ? 0.13 : weight >= 600 ? 0.115 : weight >= 500 ? 0.1 : 0.085));
    QPen pen(color, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    it->second(Ctx{p, centre, size / 2});
    p.restore();
}

QIcon icon(const QString& name, double size, const QColor& color, int weight) {
    const int px = int(std::ceil(size + 4));
    QPixmap pm(px * 2, px * 2);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    draw(p, name, QPointF(px / 2.0, px / 2.0), size, color, weight);
    p.end();
    return QIcon(pm);
}

} // namespace er::Symbols
