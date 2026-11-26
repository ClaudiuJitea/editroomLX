// ColorGrade.swift, ported literally: the grade model, GradeRecipe, built-in looks, LookMath,
// .cube files and the 32³ cube baker. The maths here is the reference for the GPU path.
#pragma once
#include "Types.h"
#include <array>
#include <functional>
#include <memory>

namespace er {

struct RGB { double r = 0, g = 0, b = 0; bool operator==(const RGB&) const = default; };

struct WheelPoint {
    double x = 0.0, y = 0.0;
    Extra extra;
    /// RGB offset this wheel position adds, before weighting by tonal range.
    RGB offset() const;
    bool operator==(const WheelPoint& o) const { return x == o.x && y == o.y && extra == o.extra; }
};

/// Per-clip colour correction: an optional look, five basic adjustments, and three tint wheels.
struct ColorGrade {
    /// A built-in look's name, or the path of a .cube file. Nil for none.
    std::optional<QString> look;
    double lookIntensity = 1.0;
    /// Stops, -2...2.
    double exposure = 0.0;
    /// -50...50.
    double contrast = 0.0;
    /// -100...100.
    double saturation = 0.0;
    /// -50...50, negative is cooler.
    double temperature = 0.0;
    /// -50...50, positive is toward magenta.
    double tint = 0.0;
    /// Wheel positions inside the unit circle: x right, y down. Red sits at 0°, green at 120°, blue at 240°.
    WheelPoint shadows, midtones, highlights;
    Extra extra;

    /// No look and every adjustment at zero. The look's strength doesn't count without a look.
    bool isNeutral() const;
    QString lookLabel() const;
    /// Equality of the model fields (what Swift's Hashable compares); `extra` is ignored.
    bool sameGrade(const ColorGrade& o) const;
    bool operator==(const ColorGrade&) const = default;
};

/// The primitive operations every look and every user grade is made of.
struct GradeRecipe {
    double exposure = 0.0, contrast = 0.0, saturation = 0.0, temperature = 0.0, tint = 0.0;
    RGB shadows, midtones, highlights, lift;
    double fade = 0.0;
    /// A look with its own maths (see LookMath), used instead of the steps above.
    RGB (*custom)(RGB) = nullptr;

    RGB apply(RGB rgb) const;
};

namespace BuiltInLook {
/// The Looks offered in the menu, in order.
const std::vector<std::pair<QString, GradeRecipe>>& all();
std::optional<GradeRecipe> named(const QString& name);
} // namespace BuiltInLook

namespace LookMath {
RGB filmPrint(RGB c);
RGB goldenHour(RGB c);
RGB bleachBypass(RGB c);
RGB pastel(RGB c);
} // namespace LookMath

/// A 3D lookup table read from a .cube file.
class CubeLUT {
public:
    static std::shared_ptr<CubeLUT> load(const QString& path);
    static std::shared_ptr<CubeLUT> parse(const QString& text);
    int size() const { return size_; }
    /// Trilinear lookup.
    RGB sample(RGB rgb) const;
private:
    int size_ = 0;
    /// size³ RGB triples, red varying fastest.
    std::vector<float> values_;
};

/// Turns a whole grade (look, then adjustments and wheels on top) into one colour cube, so the
/// viewer and the export run exactly the same single lookup per pixel.
namespace GradeBaker {
/// Points per axis in a baked cube.
inline constexpr int dimension = 32;
/// RGBA float data (n³·4, red fastest, alpha 1), or empty when the grade changes nothing.
/// Cached (at most 200 entries, as on the Mac). Thread-safe.
std::shared_ptr<const std::vector<float>> cube(const std::optional<ColorGrade>& grade);
std::shared_ptr<CubeLUT> cubeFile(const QString& path);
} // namespace GradeBaker

} // namespace er
