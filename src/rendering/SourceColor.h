// SourceColor.swift: how a video file's picture is encoded, and the 64³ cubes that turn HDR and Log
// pixel values straight into Rec.709 ones, ported literally.
#pragma once
#include <memory>
#include <tuple>
#include <vector>

namespace er {

enum class SourceColor {
    /// Rec.709 / sRGB: shown as is.
    standard,
    /// HDR with the HLG curve (iPhone HDR, including Dolby Vision 8.4).
    hlg,
    /// HDR10 / PQ.
    pq,
    /// Apple Log (iPhone 15 Pro and 16 Pro), in Rec.2020 colours.
    appleLog,
    /// Apple Log 2 (iPhone 17 Pro): the same curve, in Apple Wide Gamut colours.
    appleLog2,
    /// HLG or PQ with Dolby Vision (as iPhones write it): converted with the curve fitted to macOS's own conversion
    /// of iPhone HDR, which uses the Dolby Vision metadata. Plain HLG and PQ keep the BT.2408 mapping.
    hlgVision,
    pqVision,
};

namespace SourceColors {
inline constexpr int dimension = 64;
/// One encoded colour to encoded Rec.709, with white (diffuse, not peak) at 1.
std::tuple<double, double, double> convert(SourceColor kind, double r, double g, double b);
/// RGBA float data (64³·4, red fastest), or null for `standard`. Made once per kind.
std::shared_ptr<const std::vector<float>> cube(SourceColor kind);
const char* name(SourceColor kind);
} // namespace SourceColors

} // namespace er
