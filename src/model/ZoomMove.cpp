#include "ZoomMove.h"
#include <algorithm>

namespace er {

ZoomMove ZoomMove::starter(Mode mode, double length) {
    const double m = std::min({std::max(length * 0.25, 0.5), 2.0, length / 2});
    ZoomMove z;
    z.mode = mode;
    switch (mode) {
    case Mode::zoomIn: z.p1 = 0; z.p2 = m; z.p3 = length; z.p4 = length; break;
    case Mode::zoomOut: z.p1 = 0; z.p2 = 0; z.p3 = length - m; z.p4 = length; break;
    case Mode::inOut: z.p1 = 0; z.p2 = m; z.p3 = length - m; z.p4 = length; break;
    }
    return z;
}

double ZoomMove::amount(double u) const {
    switch (mode) {
    case Mode::zoomIn: return ramp(u, p1, p2);
    case Mode::zoomOut: return 1 - ramp(u, p3, p4);
    case Mode::inOut: return u < p3 ? ramp(u, p1, p2) : 1 - ramp(u, p3, p4);
    }
    return 0;
}

double ZoomMove::ramp(double u, double a, double b) const {
    if (!(b - a > 1e-6)) return u >= a ? 1 : 0;
    const double k = std::min(std::max((u - a) / (b - a), 0.0), 1.0);
    return smooth ? k * k * (3 - 2 * k) : k;
}

double ZoomMove::zoomedTime(double length) const {
    switch (mode) {
    case Mode::zoomIn: return std::min(std::max(p2, 0.0), std::max(0.0, length - 1e-3));
    case Mode::zoomOut: return std::min(std::max(0.0, p3 - 1e-3), std::max(0.0, length - 1e-3));
    case Mode::inOut: return std::min(std::max(p2, 0.0), std::max(0.0, length - 1e-3));
    }
    return 0;
}

void ZoomMove::fit(double length) {
    auto c = [&](double v) { return std::min(std::max(v, 0.0), length); };
    p1 = c(p1); p2 = std::max(p1, c(p2)); p3 = std::max(p2, c(p3)); p4 = std::max(p3, c(p4));
}

void ZoomMove::shift(double d, double length) {
    p1 -= d; p2 -= d; p3 -= d; p4 -= d;
    fit(length);
}

std::optional<ClipTransform> ClipTransform::at(double u) const {
    if (!zoom) {
        ClipTransform t = *this;
        std::tie(t.x, t.y) = zoomedOffset();
        if (t.isIdentity()) return std::nullopt;
        return t;
    }
    const double k = zoom->amount(u);
    ClipTransform t;
    t.scale = 1 + (scale - 1) * k;
    // The framing zooms toward a fixed point, so the offset grows with the scale; zoomed in, it never
    // shows past the picture's edge.
    const auto o = zoomedOffset();
    t.x = o.first * k;
    t.y = o.second * k;
    if (t.isIdentity()) return std::nullopt;
    return t;
}

std::pair<double, double> ClipTransform::zoomedOffset() const {
    if (!fills()) return {x, y};
    const double m = (scale - 1) / 2;
    return {std::min(std::max(x, -m), m), std::min(std::max(y, -m), m)};
}

} // namespace er
