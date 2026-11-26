#include "ExportSettings.h"
#include <cmath>

namespace er {

QString ExportSettings::label(Shape s) {
    switch (s) {
    case Shape::original: return QStringLiteral("Sequence shape");
    case Shape::vertical: return QStringLiteral("Vertical 9:16");
    case Shape::square: return QStringLiteral("Square 1:1");
    case Shape::portrait: return QStringLiteral("Portrait 4:5");
    }
    return {};
}

std::optional<double> ExportSettings::tallness(Shape s) {
    switch (s) {
    case Shape::original: return std::nullopt;
    case Shape::vertical: return 16.0 / 9.0;
    case Shape::square: return 1.0;
    case Shape::portrait: return 5.0 / 4.0;
    }
    return std::nullopt;
}

Project ExportSettings::output(const Project& project) const {
    if (!rangeOnly) return project;
    auto r = project.playRange();
    if (!r) return project;
    return project.cropped(*r);
}

std::pair<int, int> ExportSettings::renderSize(const Project& project) const {
    auto even = [](double v) { return int(std::max(2.0, std::round(v / 2) * 2)); };
    if (auto tall = tallness(shape)) {
        // The resolution names the short side; crops are never taller-than-wide in the other direction.
        const double shortSide = height ? *height : std::min(project.width, project.height);
        return {even(shortSide), even(shortSide * *tall)};
    }
    const double h = height ? *height : project.height;
    const double w = h * double(project.width) / double(std::max(1, project.height));
    return {even(w), even(h)};
}

bool ExportSettings::crops(const Project& project) const {
    const auto [w, h] = renderSize(project);
    return std::abs(double(w) / h - project.aspect()) > 0.01;
}

long long ExportSettings::videoBitrate(int w, int h, double fps) const {
    double bpp = quality == Quality::high ? 0.2 : 0.085;
    if (codec == Codec::hevc) bpp *= 0.62;
    return (long long)(double(w) * h * std::min(fps, 60.0) * bpp);
}

} // namespace er
