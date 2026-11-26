#include "SpeedRamp.h"
#include "Project.h"
#include <algorithm>
#include <cmath>

namespace er {

static double blend(double a, double b, double u) {
    const double e = u * u * (3 - 2 * u);
    return std::exp(std::log(a) + (std::log(b) - std::log(a)) * e);
}

double SpeedRamp::speed(double s, double mid) const {
    if (s < inTo) {
        if (s <= inFrom) return startSpeed;
        return blend(startSpeed, mid, (s - inFrom) / (inTo - inFrom));
    }
    if (s > outFrom) {
        if (s >= outTo) return endSpeed;
        return blend(mid, endSpeed, (s - outFrom) / (outTo - outFrom));
    }
    return mid;
}

std::pair<double, double> SpeedRamp::extremes(double mid) const {
    return {std::min({startSpeed, mid, endSpeed}), std::max({startSpeed, mid, endSpeed})};
}

double SpeedRamp::span(double s0, double s1, double mid) const {
    if (s1 < s0) return -span(s1, s0, mid);
    double total = 0.0;
    double a = s0;
    // Where the speed is constant: the pieces of footage between these marks.
    const double marks[5] = {inFrom, inTo, outFrom, outTo, s1};
    for (double m : marks) {
        if (!(m > a)) continue;
        const double b = std::min(m, s1);
        total += piece(a, b, mid);
        a = b;
        if (a >= s1) break;
    }
    return total;
}

/// One stretch with no mark inside it: exact if the speed is constant, else Simpson's rule.
double SpeedRamp::piece(double a, double b, double mid) const {
    if (!(b > a)) return 0;
    const double va = speed(a, mid), vb = speed(b, mid), vm = speed((a + b) / 2, mid);
    if (std::abs(va - vb) < 1e-12 && std::abs(va - vm) < 1e-12) return (b - a) / va;
    const int n = 24;
    const double h = (b - a) / double(n);
    double sum = 1 / va + 1 / vb;
    for (int i = 1; i < n; ++i) sum += (i % 2 == 0 ? 2 : 4) / speed(a + double(i) * h, mid);
    return sum * h / 3;
}

double SpeedRamp::source(double s0, double dt, double mid) const {
    if (std::abs(dt) < 1e-12) return s0;
    const bool forward = dt > 0;
    double left = std::abs(dt);
    double a = s0;
    std::vector<double> stops;
    for (double m : {inFrom, inTo, outFrom, outTo})
        if (forward ? m > s0 : m < s0) stops.push_back(m);
    if (forward) std::sort(stops.begin(), stops.end());
    else std::sort(stops.begin(), stops.end(), std::greater<double>());
    for (double m : stops) {
        const double t = std::abs(span(a, m, mid));
        if (t >= left) return solve(a, m, left, mid);
        left -= t;
        a = m;
    }
    // Past every mark: constant from here.
    const double v = speed(forward ? a + 1e-9 : a - 1e-9, mid);
    return a + (forward ? 1 : -1) * left * v;
}

/// Within one stretch: the point `time` timeline seconds from `a` toward `b`.
double SpeedRamp::solve(double a, double b, double time, double mid) const {
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        const double m = (lo + hi) / 2;
        if (std::abs(span(a, a + (b - a) * m, mid)) < time) lo = m; else hi = m;
    }
    return a + (b - a) * (lo + hi) / 2;
}

SpeedRamp SpeedRamp::shifted(double d) const {
    SpeedRamp r = *this;
    r.inFrom += d; r.inTo += d; r.outFrom += d; r.outTo += d;
    return r;
}

SpeedRamp SpeedRamp::mirrored(double length) const {
    SpeedRamp r = *this;
    r.inFrom = length - outTo; r.inTo = length - outFrom; r.outFrom = length - inTo; r.outTo = length - inFrom;
    r.startSpeed = endSpeed; r.endSpeed = startSpeed;
    return r;
}

std::optional<SpeedRamp> SpeedRamp::drawn(double s0, double s1, double p1, double p2, double p3, double p4,
                                          double start, double end, double mid) {
    const double L = s1 - s0;
    const bool hasIn = p2 - p1 > 0.001, hasOut = p4 - p3 > 0.001;
    if (!(hasIn || hasOut) || !(L > 0)) return std::nullopt;
    auto at = [&](double f) { return s0 + L * std::min(1.0, std::max(0.0, f)); };
    const double inFrom = hasIn ? at(p1) : s0, inTo = hasIn ? at(p2) : s0;
    const double outFrom = hasOut ? std::max(inTo, at(p3)) : s1, outTo = hasOut ? std::max(inTo, at(p4)) : s1;
    SpeedRamp r;
    r.inFrom = inFrom; r.inTo = inTo; r.outFrom = outFrom; r.outTo = outTo;
    r.startSpeed = hasIn ? start : mid;
    r.endSpeed = hasOut ? end : mid;
    return r;
}

// MARK: SourceClock and the Clip extension

SourceClock::SourceClock(const Clip& clip)
    : start(clip.start), sourceStart(clip.sourceStart), speed(clip.isFreeze() ? 0 : clip.speed),
      ramp(clip.isFreeze() ? std::nullopt : clip.ramp) {}

double SourceClock::source(double t) const {
    if (!ramp) return sourceStart + (t - start) * speed;
    return ramp->source(sourceStart, t - start, speed);
}

double Clip::speedAtSource(double s) const {
    if (!ramp || !playsSource()) return speed;
    return ramp->speed(s, speed);
}

double Clip::sourceTime(double t) const {
    if (!ramp || !playsSource()) return sourceStart + (t - start) * speed;
    return ramp->source(sourceStart, t - start, speed);
}

double Clip::sourceLength() const {
    if (!isRamped()) return duration * speed;
    return sourceTime(end()) - sourceStart;
}

double Clip::timelineSpan(double s0, double s1) const {
    if (!ramp || !playsSource()) return (s1 - s0) / speed;
    return ramp->span(s0, s1, speed);
}

std::pair<double, double> Clip::speedRange() const {
    if (!ramp || !playsSource()) return {speed, speed};
    return ramp->extremes(speed);
}

void Clip::retime(const std::function<double(double)>& snap, double minimum, const std::function<void(Clip&)>& change) {
    if (!playsSource()) { change(*this); return; }
    const double s0 = sourceStart, length = sourceLength();
    change(*this);
    duration = std::max(snap(timelineSpan(s0, s0 + length)), minimum);
    clampFades();
}

} // namespace er
