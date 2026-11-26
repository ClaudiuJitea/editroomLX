// A picture ready for the compositor: decoded video (YUV planes as the decoder gave them) or a still
// (premultiplied RGBA8). Plane data is shared, so a frame can sit in a cache and be uploaded as is.
#pragma once
#include "ColorMath.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <functional>
#include <vector>

namespace er {

/// A hardware-decoded frame still in GPU memory, as DMA-BUFs (VA-API's surfaces, exported): one layer per plane
/// (luma, then interleaved chroma), each with its DRM format, tiling modifier, offset and pitch. The compositor
/// imports it and copies it on the GPU into the same layout `bytes()` would have on the CPU; any CPU user gets it
/// downloaded once, on first ask.
struct DmaBuf {
    struct Layer {
        int fd = -1;
        uint32_t fourcc = 0;
        uint64_t modifier = 0;
        int64_t offset = 0, pitch = 0;
        int width = 0, height = 0;
        int bytesPerTexel = 1;
    };
    std::vector<Layer> layers;
    /// Keeps the decoder's surface (and the fds) alive while the frame is.
    std::shared_ptr<void> hold;
    std::function<std::shared_ptr<const std::vector<uint8_t>>()> download;
    std::shared_ptr<const std::vector<uint8_t>> downloaded() const;
private:
    mutable std::mutex lock_;
    mutable std::shared_ptr<const std::vector<uint8_t>> cpu_;
};

struct VideoFrame {
    int format = ER_SRC_RGBA8;
    int width = 0, height = 0;
    int matrix = ER_MATRIX_709;
    bool fullRange = false;
    /// EXIF orientation 1…8 (a rotated phone video becomes 6 or 8).
    int orientation = 1;
    /// Source time of this frame, in seconds.
    double time = 0;
    std::shared_ptr<const std::vector<uint8_t>> data;
    /// Set instead of `data` for a frame still on the GPU.
    std::shared_ptr<const DmaBuf> dmabuf;
    int offset[3] = {0, 0, 0};
    int stride[3] = {0, 0, 0};
    /// Identifies the pixels, so a GPU copy can be reused while it's the same frame.
    uint64_t key = 0;

    int displayWidth() const { return orientation >= 5 ? height : width; }
    int displayHeight() const { return orientation >= 5 ? width : height; }
    bool valid() const { return (data || dmabuf) && width > 0 && height > 0; }
    /// The pixels in memory (downloaded from the GPU the first time, for a DMA-BUF frame); none if that failed.
    const std::vector<uint8_t>* bytes() const;
    /// How many bytes the planes take in the layout `offset`/`stride` describe.
    size_t byteSize() const;
    /// The packed layout `yuv` copies into (each row 4-byte aligned).
    static void tightLayout(int format, int w, int h, int offset[3], int stride[3]);

    /// A frame from premultiplied RGBA8 rows.
    static VideoFrame rgba(int w, int h, std::vector<uint8_t> pixels, int stride = 0);
    /// An NV12 or P010 frame from planes (copied).
    static VideoFrame yuv(int format, int w, int h, const uint8_t* const planes[3], const int strides[3], int matrix, bool fullRange);
    static uint64_t nextKey();
};

} // namespace er
