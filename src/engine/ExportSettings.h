// ExportSettings (Exporter.swift), ported verbatim: format, size, codec, quality, loudness target,
// output shapes with reframing, in/out range, and the bitrate maths.
#pragma once
#include "model/Project.h"

namespace er {

struct ExportSettings {
    enum class Format { mp4, wav };        // "Video", "Audio"
    enum class Quality { high, web };      // "High", "Web"
    enum class Codec { h264, hevc };       // "H.264", "HEVC"
    enum class Shape { original, vertical, square, portrait };

    Format format = Format::mp4;
    /// Output height; nil keeps the sequence size.
    std::optional<int> height;
    Codec codec = Codec::h264;
    Quality quality = Quality::high;
    bool includeAudio = true;
    /// Target integrated loudness in LUFS; nil leaves levels as they are.
    std::optional<double> loudness;
    /// The output's shape. Anything but the sequence's own crops the picture to fit.
    Shape shape = Shape::original;
    /// Which part of the picture to keep when cropping, as the crop's centre (0…1 across and down).
    double reframeX = 0.5, reframeY = 0.5;
    /// Export just the in/out range, when the timeline has one.
    bool rangeOnly = false;

    bool operator==(const ExportSettings&) const = default;

    static QString label(Format f) { return f == Format::mp4 ? QStringLiteral("Video") : QStringLiteral("Audio"); }
    static QString label(Quality q) { return q == Quality::high ? QStringLiteral("High") : QStringLiteral("Web"); }
    static QString label(Codec c) { return c == Codec::h264 ? QStringLiteral("H.264") : QStringLiteral("HEVC"); }
    static QString label(Shape s);
    /// Height over width, or nil for the sequence's own shape.
    static std::optional<double> tallness(Shape s);

    /// What gets exported: the whole timeline, or just its in/out range moved to start at zero.
    Project output(const Project& project) const;
    std::pair<int, int> renderSize(const Project& project) const;
    /// Whether the output crops the sequence's picture.
    bool crops(const Project& project) const;
    long long videoBitrate(int w, int h, double fps) const;
    QString fileExtension() const { return format == Format::mp4 ? QStringLiteral("mp4") : QStringLiteral("wav"); }
};

} // namespace er
