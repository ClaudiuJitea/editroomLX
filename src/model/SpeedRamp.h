// SpeedRamp.swift, ported literally.
#pragma once
#include "Types.h"

namespace er {

/// A clip's speed changing over its footage: from `startSpeed`, easing into the clip's own speed
/// between `inFrom` and `inTo`, then easing out to `endSpeed` between `outFrom` and `outTo`. Times are
/// in the clip's source (seconds of footage), so a trim or a split just shows a different part of the
/// same curve, and each half of a split keeps its piece.
struct SpeedRamp {
    double inFrom = 0, inTo = 0, outFrom = 0, outTo = 0;
    double startSpeed = 1, endSpeed = 1;
    Extra extra;

    /// Speed at a point in the footage, with `mid` the clip's own speed. Eased in log speed, so going
    /// from 100% to 400% feels as even as 400% to 100%.
    double speed(double s, double mid) const;
    /// The fastest and slowest it plays.
    std::pair<double, double> extremes(double mid) const;
    /// Timeline seconds to play footage from `s0` to `s1` (negative if `s1` is before `s0`).
    double span(double s0, double s1, double mid) const;
    /// The footage reached after playing `dt` timeline seconds from `s0` (backwards if `dt` is negative).
    double source(double s0, double dt, double mid) const;
    /// The same curve with the footage measured from somewhere else (`d` seconds later).
    SpeedRamp shifted(double d) const;
    /// The same curve measured from the other end of a file `length` long (for reversing a clip).
    SpeedRamp mirrored(double length) const;
    /// A ramp drawn over a clip's footage `s0...s1`, as the inspector's graph sets it.
    static std::optional<SpeedRamp> drawn(double s0, double s1, double p1, double p2, double p3, double p4,
                                          double start, double end, double mid);

    bool operator==(const SpeedRamp&) const = default;

private:
    double piece(double a, double b, double mid) const;
    double solve(double a, double b, double time, double mid) const;
};

} // namespace er
