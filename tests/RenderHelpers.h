// Pictures and plans for the renderer tests.
#pragma once
#include "rendering/RenderPlan.h"
#include <cmath>
#include <map>

namespace er::test {

struct FakeMedia : MediaInfo {
    std::map<Uuid, MediaRuntime> runtimes;
    std::map<Uuid, Transcript> transcripts;
    const MediaRuntime* runtime(const Uuid& id) const override {
        auto it = runtimes.find(id);
        return it == runtimes.end() ? nullptr : &it->second;
    }
    std::optional<Transcript> transcript(const Uuid& id) const override {
        auto it = transcripts.find(id);
        if (it == transcripts.end()) return std::nullopt;
        return it->second;
    }
};

/// An RGBA8 picture filled by a function of (x, y) → premultiplied 0…255 values.
template <class F> VideoFrame picture(int w, int h, F f) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto c = f(x, y);
            for (int k = 0; k < 4; ++k) px[(size_t(y) * w + x) * 4 + k] = uint8_t(c[k]);
        }
    return VideoFrame::rgba(w, h, std::move(px));
}

/// An NV12 (or P010 when `bits` is 10) frame with a gradient and colour patches, video range.
inline VideoFrame gradientYuv(int w, int h, int bits, int matrix = ER_MATRIX_709) {
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int bpp = bits > 8 ? 2 : 1;
    std::vector<uint8_t> y(size_t(w) * h * bpp), uv(size_t(cw) * ch * 2 * bpp);
    auto put = [&](std::vector<uint8_t>& v, size_t i, int code) {
        if (bpp == 1) v[i] = uint8_t(code);
        else { const int c = code << 6; v[i * 2] = uint8_t(c & 255); v[i * 2 + 1] = uint8_t(c >> 8); }
    };
    const int lo = bits > 8 ? 64 : 16, hi = bits > 8 ? 940 : 235, mid = bits > 8 ? 512 : 128, span = bits > 8 ? 448 : 112;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) put(y, size_t(j) * w + i, lo + (hi - lo) * i / std::max(1, w - 1));
    for (int j = 0; j < ch; ++j)
        for (int i = 0; i < cw; ++i) {
            put(uv, (size_t(j) * cw + i) * 2, mid + int(span * std::sin(i * 0.3)));
            put(uv, (size_t(j) * cw + i) * 2 + 1, mid + int(span * std::cos(j * 0.2)));
        }
    const uint8_t* planes[3] = {y.data(), uv.data(), nullptr};
    const int strides[3] = {w * bpp, cw * 2 * bpp, 0};
    return VideoFrame::yuv(bits > 8 ? ER_SRC_P010 : ER_SRC_NV12, w, h, planes, strides, matrix, false);
}

inline double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int* maxDelta) {
    double se = 0;
    int md = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int d = std::abs(int(a[i]) - int(b[i]));
        md = std::max(md, d);
        se += double(d) * d;
    }
    if (maxDelta) *maxDelta = md;
    const double mse = se / double(std::max<size_t>(1, a.size()));
    return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

} // namespace er::test
