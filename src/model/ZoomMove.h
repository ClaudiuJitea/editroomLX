// ZoomMove.swift and ClipTransform (Project.swift), ported literally.
#pragma once
#include "Types.h"

namespace er {

/// A zoom over a clip, set on its Scale: the picture moves between the full frame and the clip's Scale and
/// position (the zoomed-in framing), in, out, or in and back out.
struct ZoomMove {
    enum class Mode { zoomIn /* "in" */, inOut, zoomOut /* "out" */ };
    Mode mode = Mode::zoomIn;
    /// Seconds from the clip's start: the move in runs `p1...p2`, the move out `p3...p4`.
    double p1 = 0, p2 = 0, p3 = 0, p4 = 0;
    /// Eased at both ends of each move; otherwise at an even pace.
    bool smooth = true;
    Extra extra;

    /// Mode last picked, which new zooms start in (a settings key).
    static constexpr const char* modeKey = "zoomMoveMode";

    static ZoomMove starter(Mode mode, double length);
    /// How far in at `u` seconds into the clip: 0 is the full frame, 1 the clip's Scale.
    double amount(double u) const;
    /// Where it's all the way in, for showing the zoomed framing.
    double zoomedTime(double length) const;
    /// Kept within a clip `length` long and in order.
    void fit(double length);
    /// After the clip's start moves later by `d` seconds (its head trimmed), so the zoom stays on its picture.
    void shift(double d, double length);

    bool operator==(const ZoomMove&) const = default;

private:
    double ramp(double u, double a, double b) const;
};

/// Scale and position of a clip within the frame.
struct ClipTransform {
    /// 1 is fitted to the frame.
    double scale = 1.0;
    /// Offset of the clip's centre from the frame's centre, as a fraction of the frame's width and
    /// height. Positive y is down.
    double x = 0.0, y = 0.0;
    /// Moving between the full frame and this framing over the clip (the zoom button on Scale).
    std::optional<ZoomMove> zoom;
    Extra extra;

    static constexpr double scaleMin = 0.05, scaleMax = 10.0;
    /// Note: compares the model fields only, like Swift's `self == ClipTransform()`.
    bool isIdentity() const { return scale == 1.0 && x == 0.0 && y == 0.0 && !zoom; }
    /// The framing at `u` seconds into the clip, with any zoom applied; nil when that's the plain full frame.
    std::optional<ClipTransform> at(double u) const;
    /// Bigger than the frame: placed by where it frames, never past its edges.
    bool fills() const { return scale > 1.001; }
    /// The offset when all the way in, kept so a picture bigger than the frame still covers it.
    std::pair<double, double> zoomedOffset() const;

    bool operator==(const ClipTransform&) const = default;
};

} // namespace er
