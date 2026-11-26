#include "CpuRenderer.h"
#include <cstring>

namespace er::cm {

/// Kernel.inc inside a struct, so its fetches read this frame's data.
struct CpuKernel {
    const FrameJob& job;
    std::vector<const uint8_t*> base;

    explicit CpuKernel(const FrameJob& j) : job(j) {
        for (const auto& p : j.pictures) { const auto* b = p.bytes(); base.push_back(b ? b->data() : nullptr); }
    }
    ErFrame er_frame() const { return job.frame; }
    ErLayer er_layer(int i) const { return job.layers[size_t(i)]; }
    ErSource er_source(int i) const { ErSource s = job.sources[size_t(i)]; s.pad0 = i; return s; }
    int er_byte(const ErSource& s, int o) const { return base[size_t(s.pad0)][o]; }
    int er_u16(const ErSource& s, int o) const { const uint8_t* b = base[size_t(s.pad0)]; return int(b[o]) | (int(b[o + 1]) << 8); }
    ivec2 er_u16x2(const ErSource& s, int o) const { return ivec2(er_u16(s, o), er_u16(s, o + 2)); }
    float er_cubef(int i) const { return job.cubes[size_t(i)]; }

#include "Kernel.inc"
};

} // namespace er::cm

namespace er {

std::vector<float> CpuRenderer::renderFloat(const FrameJob& job) {
    const int w = job.frame.outW, h = job.frame.outH;
    std::vector<float> out(size_t(w) * h * 4);
#pragma omp parallel for schedule(dynamic, 4)
    for (int y = 0; y < h; ++y) {
        cm::CpuKernel k(job);
        for (int x = 0; x < w; ++x) {
            const cm::vec4 c = k.er_output_pixel(x, y);
            float* p = &out[(size_t(y) * w + x) * 4];
            p[0] = c.x; p[1] = c.y; p[2] = c.z; p[3] = c.w;
        }
    }
    return out;
}

std::vector<uint8_t> CpuRenderer::render(const FrameJob& job) {
    const auto f = renderFloat(job);
    std::vector<uint8_t> out(f.size());
#pragma omp parallel for
    for (long long i = 0; i < (long long)(f.size() / 4); ++i) {
        out[size_t(i) * 4] = uint8_t(cm::er_to8(f[size_t(i) * 4]));
        out[size_t(i) * 4 + 1] = uint8_t(cm::er_to8(f[size_t(i) * 4 + 1]));
        out[size_t(i) * 4 + 2] = uint8_t(cm::er_to8(f[size_t(i) * 4 + 2]));
        out[size_t(i) * 4 + 3] = 255;
    }
    return out;
}

std::vector<uint8_t> CpuRenderer::renderNV12(const FrameJob& job) {
    const int w = job.frame.outW, h = job.frame.outH;
    const auto f = renderFloat(job);
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<uint8_t> out(size_t(w) * h + size_t(cw) * ch * 2);
#pragma omp parallel for
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float* p = &f[(size_t(y) * w + x) * 4];
            const cm::vec3 yuv = cm::er_rgb_to_yuv709(cm::vec3(p[0], p[1], p[2]), 255.0f);
            out[size_t(y) * w + x] = uint8_t(cm::clamp(int(cm::floor(yuv.x + 0.5f)), 0, 255));
        }
#pragma omp parallel for
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            // The 2×2 block's average colour.
            cm::vec3 sum(0.0f);
            float n = 0;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const int px = std::min(w - 1, x * 2 + i), py = std::min(h - 1, y * 2 + j);
                    const float* p = &f[(size_t(py) * w + px) * 4];
                    sum = sum + cm::vec3(cm::er_clamp01(p[0]), cm::er_clamp01(p[1]), cm::er_clamp01(p[2]));
                    n += 1;
                }
            const cm::vec3 yuv = cm::er_rgb_to_yuv709(sum / n, 255.0f);
            uint8_t* c = &out[size_t(w) * h + (size_t(y) * cw + x) * 2];
            c[0] = uint8_t(cm::clamp(int(cm::floor(yuv.y + 0.5f)), 0, 255));
            c[1] = uint8_t(cm::clamp(int(cm::floor(yuv.z + 0.5f)), 0, 255));
        }
    return out;
}

} // namespace er
