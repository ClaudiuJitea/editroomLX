// Still images (ImageIO on the Mac): JPEG, PNG, TIFF, WebP… through Qt's image plugins, HEIC/HEIF/AVIF
// through libheif. EXIF orientation is applied, and the picture is kept no bigger than 3840 on its long
// side (CGImageSourceCreateThumbnailAtIndex with kCGImageSourceThumbnailMaxPixelSize 3840).
#pragma once
#include "rendering/VideoFrame.h"
#include <QImage>
#include <QString>
#include <optional>

namespace er::Stills {
/// The size as the Mac's import reads it: pixels, swapped for EXIF orientations 5–8.
std::optional<std::pair<int, int>> size(const QString& path);
/// The picture, oriented, premultiplied, at most `maxSide` on its long side.
std::optional<QImage> load(const QString& path, int maxSide = 3840);
/// The same as a compositor frame (premultiplied RGBA8).
std::optional<VideoFrame> frame(const QString& path, int maxSide = 3840);
VideoFrame toFrame(const QImage& image);
bool isImage(const QString& path);
} // namespace er::Stills
