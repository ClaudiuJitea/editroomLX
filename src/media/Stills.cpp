#include "Stills.h"
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <cstring>
#include <libheif/heif.h>

namespace er::Stills {

static bool isHeif(const QString& path) {
    const QString e = QFileInfo(path).suffix().toLower();
    return e == QLatin1String("heic") || e == QLatin1String("heif") || e == QLatin1String("hif") || e == QLatin1String("avif");
}

bool isImage(const QString& path) {
    if (isHeif(path)) return true;
    static QMimeDatabase db;
    return db.mimeTypeForFile(path, QMimeDatabase::MatchExtension).name().startsWith(QLatin1String("image/"));
}

static std::optional<QImage> loadHeif(const QString& path, bool sizeOnly, int* w = nullptr, int* h = nullptr) {
    heif_context* ctx = heif_context_alloc();
    std::optional<QImage> out;
    if (heif_context_read_from_file(ctx, path.toUtf8().constData(), nullptr).code == heif_error_Ok) {
        heif_image_handle* handle = nullptr;
        if (heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok) {
            // libheif applies the file's rotation and mirroring (irot/imir) when decoding.
            if (w) *w = heif_image_handle_get_width(handle);
            if (h) *h = heif_image_handle_get_height(handle);
            if (!sizeOnly) {
                heif_image* img = nullptr;
                const bool alpha = heif_image_handle_has_alpha_channel(handle);
                if (heif_decode_image(handle, &img, heif_colorspace_RGB, alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, nullptr).code == heif_error_Ok) {
                    int stride = 0;
                    const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
                    const int iw = heif_image_get_width(img, heif_channel_interleaved), ih = heif_image_get_height(img, heif_channel_interleaved);
                    QImage q(data, iw, ih, stride, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
                    out = q.copy();
                    heif_image_release(img);
                }
            }
            heif_image_handle_release(handle);
        }
    }
    heif_context_free(ctx);
    if (sizeOnly && w && *w > 0) return QImage();
    return out;
}

std::optional<std::pair<int, int>> size(const QString& path) {
    if (isHeif(path)) {
        int w = 0, h = 0;
        loadHeif(path, true, &w, &h);
        if (w <= 0 || h <= 0) return std::nullopt;
        return std::make_pair(w, h);
    }
    QImageReader r(path);
    r.setAutoTransform(true);
    QSize s = r.size();
    if (!s.isValid()) return std::nullopt;
    const auto t = r.transformation();
    if (t & QImageIOHandler::TransformationRotate90) s.transpose();
    return std::make_pair(s.width(), s.height());
}

std::optional<QImage> load(const QString& path, int maxSide) {
    QImage img;
    if (isHeif(path)) {
        auto h = loadHeif(path, false);
        if (!h) return std::nullopt;
        img = *h;
    } else {
        QImageReader r(path);
        r.setAutoTransform(true);
        img = r.read();
        if (img.isNull()) return std::nullopt;
    }
    if (maxSide > 0 && std::max(img.width(), img.height()) > maxSide)
        img = img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

VideoFrame toFrame(const QImage& image) {
    const QImage c = image.format() == QImage::Format_RGBA8888_Premultiplied ? image : image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    std::vector<uint8_t> px(size_t(c.width()) * c.height() * 4);
    for (int y = 0; y < c.height(); ++y) std::memcpy(px.data() + size_t(y) * c.width() * 4, c.constScanLine(y), size_t(c.width()) * 4);
    return VideoFrame::rgba(c.width(), c.height(), std::move(px));
}

std::optional<VideoFrame> frame(const QString& path, int maxSide) {
    auto img = load(path, maxSide);
    if (!img) return std::nullopt;
    return toFrame(*img);
}

} // namespace er::Stills
