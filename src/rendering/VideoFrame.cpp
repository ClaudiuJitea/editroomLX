#include "VideoFrame.h"
#include <atomic>
#include <cstring>

namespace er {

uint64_t VideoFrame::nextKey() {
    static std::atomic<uint64_t> k{1};
    return k++;
}

VideoFrame VideoFrame::rgba(int w, int h, std::vector<uint8_t> pixels, int stride) {
    VideoFrame f;
    f.format = ER_SRC_RGBA8;
    f.width = w;
    f.height = h;
    f.stride[0] = stride ? stride : w * 4;
    f.data = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
    f.key = nextKey();
    return f;
}

static int align4(int v) { return (v + 3) & ~3; }

std::shared_ptr<const std::vector<uint8_t>> DmaBuf::downloaded() const {
    std::lock_guard g(lock_);
    if (!cpu_ && download) cpu_ = download();
    return cpu_;
}

const std::vector<uint8_t>* VideoFrame::bytes() const {
    if (data) return data.get();
    if (dmabuf) if (auto d = dmabuf->downloaded()) return d.get();
    return nullptr;
}

void VideoFrame::tightLayout(int format, int w, int h, int offset[3], int stride[3]) {
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const bool wide = format == ER_SRC_P010 || format == ER_SRC_YUV420P10;
    const int bpp = wide ? 2 : 1;
    const int rows[3] = {h, ch, ch};
    int bytes[3] = {w * bpp, 0, 0};
    int count = 2;
    if (format == ER_SRC_NV12 || format == ER_SRC_P010) bytes[1] = cw * 2 * bpp;
    else { bytes[1] = bytes[2] = cw * bpp; count = 3; }
    size_t total = 0;
    for (int p = 0; p < 3; ++p) { offset[p] = 0; stride[p] = 0; }
    for (int p = 0; p < count; ++p) {
        offset[p] = int(total);
        stride[p] = align4(bytes[p]);
        total += size_t(stride[p]) * rows[p];
    }
}

size_t VideoFrame::byteSize() const {
    if (data) return data->size();
    if (format == ER_SRC_RGBA8) return size_t(stride[0]) * height;
    const int ch = (height + 1) / 2;
    const bool three = format == ER_SRC_YUV420P || format == ER_SRC_YUV420P10;
    return three ? size_t(offset[2]) + size_t(stride[1]) * ch : size_t(offset[1]) + size_t(stride[1]) * ch;
}

VideoFrame VideoFrame::yuv(int format, int w, int h, const uint8_t* const planes[3], const int strides[3], int matrix, bool fullRange) {
    VideoFrame f;
    f.format = format;
    f.width = w;
    f.height = h;
    f.matrix = matrix;
    f.fullRange = fullRange;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const bool wide = format == ER_SRC_P010 || format == ER_SRC_YUV420P10;
    const int bpp = wide ? 2 : 1;
    int rows[3] = {h, ch, ch};
    int bytes[3] = {w * bpp, 0, 0};
    int count = 2;
    if (format == ER_SRC_NV12 || format == ER_SRC_P010) { bytes[1] = cw * 2 * bpp; }
    else { bytes[1] = bytes[2] = cw * bpp; count = 3; }
    tightLayout(format, w, h, f.offset, f.stride);
    auto data = std::make_shared<std::vector<uint8_t>>();
    data->resize(f.byteSize());
    for (int p = 0; p < count; ++p)
        for (int y = 0; y < rows[p]; ++y)
            std::memcpy(data->data() + f.offset[p] + size_t(y) * f.stride[p], planes[p] + size_t(y) * strides[p], size_t(bytes[p]));
    f.data = data;
    f.key = nextKey();
    return f;
}

} // namespace er
