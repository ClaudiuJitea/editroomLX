// ColorMath.h — the written-down maths shared by the CPU renderer (C++) and the Vulkan shader (GLSL).
//
// This file is valid C++20 and valid GLSL 4.50 at once. C++ gets small vec types below; GLSL has its own.
// Rules for code in here (and in Kernel.inc): no swizzles (use .x .y .z .w), no GLSL-only built-ins
// beyond those defined for C++ below, `ER_OUT(T)` for out-parameters, scalar-only structs (std430 with
// 4-byte alignment matches C++ layout exactly).
//
// What it reproduces (TimelineCompositor.compose + Core Image, unmanaged): YUV→RGB with the frame's
// matrix and range, EXIF orientation, bilinear sampling with transparent outside the image (Core Image's
// affine transforms), CIColorCube's trilinear lookup with the input clamped to 0…1, premultiplied
// source-over, and BT.709 limited-range YUV packing for the encoder.
#ifndef ER_COLORMATH_H
#define ER_COLORMATH_H

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace er::cm {

struct vec2 { float x = 0, y = 0; vec2() = default; constexpr vec2(float a, float b) : x(a), y(b) {} explicit constexpr vec2(float a) : x(a), y(a) {} };
struct vec3 { float x = 0, y = 0, z = 0; vec3() = default; constexpr vec3(float a, float b, float c) : x(a), y(b), z(c) {} explicit constexpr vec3(float a) : x(a), y(a), z(a) {} };
struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    vec4() = default;
    constexpr vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    explicit constexpr vec4(float a) : x(a), y(a), z(a), w(a) {}
    constexpr vec4(vec3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
};
struct ivec2 { int x = 0, y = 0; ivec2() = default; constexpr ivec2(int a, int b) : x(a), y(b) {} };
struct ivec3 { int x = 0, y = 0, z = 0; ivec3() = default; constexpr ivec3(int a, int b, int c) : x(a), y(b), z(c) {} };

#define ER_VOP(T, OP) \
    inline T operator OP(T a, T b) { T r = a; float* p = &r.x; const float* q = &b.x; for (unsigned i = 0; i < sizeof(T) / 4; ++i) p[i] = p[i] OP q[i]; return r; } \
    inline T operator OP(T a, float s) { T r = a; float* p = &r.x; for (unsigned i = 0; i < sizeof(T) / 4; ++i) p[i] = p[i] OP s; return r; } \
    inline T operator OP(float s, T a) { T r = a; float* p = &r.x; for (unsigned i = 0; i < sizeof(T) / 4; ++i) p[i] = s OP p[i]; return r; }
ER_VOP(vec2, +) ER_VOP(vec2, -) ER_VOP(vec2, *) ER_VOP(vec2, /)
ER_VOP(vec3, +) ER_VOP(vec3, -) ER_VOP(vec3, *) ER_VOP(vec3, /)
ER_VOP(vec4, +) ER_VOP(vec4, -) ER_VOP(vec4, *) ER_VOP(vec4, /)
#undef ER_VOP

inline float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline float clamp(float v, double lo, double hi) { return clamp(v, float(lo), float(hi)); }
inline int clamp(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline vec4 mix(vec4 a, vec4 b, float t) { return a + (b - a) * t; }
inline vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float floor(float v) { return std::floor(v); }
inline float min(float a, float b) { return std::min(a, b); }
inline float max(float a, float b) { return std::max(a, b); }
inline int min(int a, int b) { return std::min(a, b); }
inline int max(int a, int b) { return std::max(a, b); }
inline float abs(float v) { return std::abs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float exp(float v) { return std::exp(v); }
inline float log(float v) { return std::log(v); }

#define ER_OUT(T) T&
#define ER_INOUT(T) T&
#define ER_INLINE inline

#else // GLSL

#define ER_OUT(T) out T
#define ER_INOUT(T) inout T
#define ER_INLINE

#endif

// MARK: Layer and source descriptors (std430, scalars only)

#define ER_SRC_RGBA8 0      // premultiplied RGBA, 4 bytes per pixel
#define ER_SRC_NV12 1       // 8-bit Y plane, interleaved CbCr plane at half size
#define ER_SRC_P010 2       // 16-bit little-endian Y and CbCr planes, value in the top 10 bits
#define ER_SRC_YUV420P 3    // 8-bit Y, Cb, Cr planes
#define ER_SRC_YUV420P10 4  // 16-bit little-endian Y, Cb, Cr planes, value in the bottom 10 bits

#define ER_MATRIX_709 0
#define ER_MATRIX_601 1
#define ER_MATRIX_2020 2

// What a layer's values mean, in an HDR project (ErLayer.transfer). An SDR project converts nothing.
#define ER_TF_709 0        // Rec.709 video (and Log, already turned into it by its cube): BT.709's curve and primaries
#define ER_TF_SRGB 1       // solids, titles, captions and photos: sRGB
#define ER_TF_HLG 2        // HLG in Rec.2020 (HDR video kept as HDR)
#define ER_TF_PQ 3         // PQ in Rec.2020 (made HLG on the way out, as AVFoundation does)

// The frame's dynamic range (ErFrame.hdr).
#define ER_HDR_OFF 0       // SDR (Rec.709), unmanaged, as every project was before
#define ER_HDR_HLG 1       // HDR: put together in linear Rec.2020 and written as HLG (the export)
#define ER_HDR_VIEW 2      // the same composite, shown on a screen without HDR: Rec.709 with highlights clipped at white

#define ER_LAYER_PICTURE 0 // a video frame or a still, fitted to the canvas
#define ER_LAYER_SOLID 1
#define ER_LAYER_PIXMAP 2  // text: a premultiplied RGBA8 image at a whole-pixel position

struct ErSource {
    int format;
    int width;      // stored size, before orientation
    int height;
    int offset0;    // byte offsets of the planes in the source buffer
    int offset1;
    int offset2;
    int stride0;    // bytes per row
    int stride1;
    int matrix;
    int fullRange;
    int orientation; // EXIF 1…8
    int pad0;
};

struct ErLayer {
    int kind;
    int source;      // ErSource index (picture, pixmap)
    int gradeCube;   // float offset of a 32³ RGBA cube in the cube buffer, or -1
    int colorCube;   // float offset of a 64³ RGBA cube (SourceColor), or -1
    float opacity;
    float wipe;      // fraction of the width shown from the left, or -1 for none
    float fitScale;  // canvas = oriented source × fitScale + (fitX, fitY), y down
    float fitX;
    float fitY;
    float scale;     // the clip's transform at this time: scale about the canvas centre…
    float offsetX;   // …then offset by this many canvas pixels (y down)
    float offsetY;
    float r;         // solid colour, premultiplied
    float g;
    float b;
    float a;
    int pixX;        // pixmap placement (top-left, canvas pixels, y down)
    int pixY;
    int upscale;     // 1: enlarged with the spatial upscaler (export), not just stretched
    int transfer;    // ER_TF_*: what its values mean in an HDR project
};

struct ErFrame {
    int canvasW;
    int canvasH;
    int outW;
    int outH;
    float offsetX;   // the output's window in the canvas (reframe), y down
    float offsetY;
    int layerCount;
    int captionCount; // pixmap layers in output space, after the canvas ones
    int firstCaption;
    int hdr;         // ER_HDR_*
    int pad1;
    int pad2;
};

// MARK: Maths

ER_INLINE float er_clamp01(float v) { return clamp(v, 0.0, 1.0); }

/// Code values (normalised to 0…1 of the format's full code range) to R'G'B' (still encoded), as the
/// decoder's colour attachments say: matrix (Kr, Kb) and video (16–235/240) or full range.
ER_INLINE vec3 er_yuv_to_rgb(float yc, float cb, float cr, int matrix, int fullRange, float maxCode) {
    float kr = 0.2126;
    float kb = 0.0722;
    if (matrix == ER_MATRIX_601) { kr = 0.299; kb = 0.114; }
    if (matrix == ER_MATRIX_2020) { kr = 0.2627; kb = 0.0593; }
    float kg = 1.0 - kr - kb;
    float k = (maxCode + 1.0) / 256.0;
    float y;
    float u;
    float v;
    if (fullRange != 0) {
        y = yc;
        u = (cb * maxCode - 128.0 * k) / maxCode;
        v = (cr * maxCode - 128.0 * k) / maxCode;
    } else {
        y = (yc * maxCode - 16.0 * k) / (219.0 * k);
        u = (cb * maxCode - 128.0 * k) / (224.0 * k);
        v = (cr * maxCode - 128.0 * k) / (224.0 * k);
    }
    float r = y + 2.0 * (1.0 - kr) * v;
    float b = y + 2.0 * (1.0 - kb) * u;
    float g = (y - kr * r - kb * b) / kg;
    return vec3(r, g, b);
}

/// Display (oriented) pixel → stored pixel, for EXIF orientations 1…8. `w`, `h` are the stored size.
ER_INLINE ivec2 er_orient(int u, int v, int orientation, int w, int h) {
    if (orientation == 2) return ivec2(w - 1 - u, v);
    if (orientation == 3) return ivec2(w - 1 - u, h - 1 - v);
    if (orientation == 4) return ivec2(u, h - 1 - v);
    if (orientation == 5) return ivec2(v, u);
    if (orientation == 6) return ivec2(v, h - 1 - u);
    if (orientation == 7) return ivec2(w - 1 - v, h - 1 - u);
    if (orientation == 8) return ivec2(w - 1 - v, u);
    return ivec2(u, v);
}

ER_INLINE bool er_swaps(int orientation) { return orientation >= 5 && orientation <= 8; }

/// The four taps of a bilinear sample at continuous position (x, y) in pixel units (centres at +0.5).
ER_INLINE void er_bilinear_taps(float x, float y, ER_OUT(ivec2) p0, ER_OUT(vec2) f) {
    float fx = x - 0.5;
    float fy = y - 0.5;
    float x0 = floor(fx);
    float y0 = floor(fy);
    p0 = ivec2(int(x0), int(y0));
    f = vec2(fx - x0, fy - y0);
}

ER_INLINE vec4 er_bilerp(vec4 a, vec4 b, vec4 c, vec4 d, vec2 f) {
    // a = (x0, y0), b = (x1, y0), c = (x0, y1), d = (x1, y1)
    vec4 top = a * (1.0 - f.x) + b * f.x;
    vec4 bottom = c * (1.0 - f.x) + d * f.x;
    return top * (1.0 - f.y) + bottom * f.y;
}

/// CIColorCube's lattice: the input clamped to 0…1, the cell and the position inside it.
ER_INLINE void er_cube_cell(vec3 c, int n, ER_OUT(ivec3) i0, ER_OUT(vec3) f) {
    float s = float(n - 1);
    float pr = er_clamp01(c.x) * s;
    float pg = er_clamp01(c.y) * s;
    float pb = er_clamp01(c.z) * s;
    int ir = min(n - 2, int(pr));
    int ig = min(n - 2, int(pg));
    int ib = min(n - 2, int(pb));
    i0 = ivec3(ir, ig, ib);
    f = vec3(pr - float(ir), pg - float(ig), pb - float(ib));
}

/// The cube's index (in RGBA texels, red fastest) of a lattice point.
ER_INLINE int er_cube_index(int r, int g, int b, int n) { return (b * n + g) * n + r; }

ER_INLINE vec3 er_trilerp(vec3 c000, vec3 c100, vec3 c010, vec3 c110, vec3 c001, vec3 c101, vec3 c011, vec3 c111, vec3 f) {
    vec3 c00 = c000 * (1.0 - f.x) + c100 * f.x;
    vec3 c10 = c010 * (1.0 - f.x) + c110 * f.x;
    vec3 c01 = c001 * (1.0 - f.x) + c101 * f.x;
    vec3 c11 = c011 * (1.0 - f.x) + c111 * f.x;
    return (c00 * (1.0 - f.y) + c10 * f.y) * (1.0 - f.z) + (c01 * (1.0 - f.y) + c11 * f.y) * f.z;
}

ER_INLINE vec4 er_over(vec4 src, vec4 dst) { return src + dst * (1.0 - src.w); }

ER_INLINE vec3 er_unpremultiply(vec4 c) {
    if (c.w <= 0.0) return vec3(0.0);
    return vec3(c.x / c.w, c.y / c.w, c.z / c.w);
}

/// 0…1 to an 8-bit code, rounding half up.
ER_INLINE int er_to8(float v) { return int(floor(er_clamp01(v) * 255.0 + 0.5)); }

/// R'G'B' to BT.709 limited-range Y'CbCr, as code values over `maxCode` (255 or 1023).
ER_INLINE vec3 er_rgb_to_yuv709(vec3 c, float maxCode) {
    float kr = 0.2126;
    float kb = 0.0722;
    float kg = 1.0 - kr - kb;
    float k = (maxCode + 1.0) / 256.0;
    float r = er_clamp01(c.x);
    float g = er_clamp01(c.y);
    float b = er_clamp01(c.z);
    float y = kr * r + kg * g + kb * b;
    float u = (b - y) / (2.0 * (1.0 - kb));
    float v = (r - y) / (2.0 * (1.0 - kr));
    return vec3(16.0 * k + 219.0 * k * y, 128.0 * k + 224.0 * k * u, 128.0 * k + 224.0 * k * v);
}

/// R'G'B' to BT.2020 (non-constant luminance) limited-range Y'CbCr, as code values over `maxCode` (an HDR export).
ER_INLINE vec3 er_rgb_to_yuv2020(vec3 c, float maxCode) {
    float kr = 0.2627;
    float kb = 0.0593;
    float kg = 1.0 - kr - kb;
    float k = (maxCode + 1.0) / 256.0;
    float r = er_clamp01(c.x);
    float g = er_clamp01(c.y);
    float b = er_clamp01(c.z);
    float y = kr * r + kg * g + kb * b;
    float u = (b - y) / (2.0 * (1.0 - kb));
    float v = (r - y) / (2.0 * (1.0 - kr));
    return vec3(16.0 * k + 219.0 * k * y, 128.0 * k + 224.0 * k * u, 128.0 * k + 224.0 * k * v);
}

// MARK: HDR projects
//
// The Mac puts an HDR project together with Core Image colour-managed, in extended linear Rec.2020 at half float:
// SDR clips, titles and solids at standard white (BT.2408: HLG 0.75), HDR highlights above it, out as HLG. Here the
// working space is the same: linear light in Rec.2020 primaries, display-referred, with SDR white (203 cd/m², HLG
// 0.75) at 1.0. HLG is BT.2100's, with its system gamma (1.2 at a 1000 cd/m² peak) applied on luminance.

/// BT.709's curve, as an encoded value to linear (and back): the itur_709 colour space Core Image matches from.
ER_INLINE float er_709_to_linear(float v) {
    if (v < 0.081) return v / 4.5;
    return pow((max(v, 0.0) + 0.099) / 1.099, 1.0 / 0.45);
}
ER_INLINE float er_linear_to_709(float v) {
    if (v < 0.018) return max(v, 0.0) * 4.5;
    return 1.099 * pow(v, 0.45) - 0.099;
}
ER_INLINE float er_srgb_to_linear(float v) {
    if (v <= 0.04045) return v / 12.92;
    return pow((v + 0.055) / 1.055, 2.4);
}

/// Rec.709 primaries to Rec.2020 (BT.2087), and back.
ER_INLINE vec3 er_709_to_2020(vec3 c) {
    return vec3(0.627404 * c.x + 0.329283 * c.y + 0.043313 * c.z,
                0.069097 * c.x + 0.919540 * c.y + 0.011362 * c.z,
                0.016391 * c.x + 0.088013 * c.y + 0.895595 * c.z);
}
ER_INLINE vec3 er_2020_to_709(vec3 c) {
    return vec3(1.660491 * c.x - 0.587641 * c.y - 0.072850 * c.z,
                -0.124551 * c.x + 1.132900 * c.y - 0.008349 * c.z,
                -0.018151 * c.x - 0.100579 * c.y + 1.118730 * c.z);
}

/// HLG's OETF and its inverse (BT.2100): scene light 0…1 to signal 0…1.
ER_INLINE float er_hlg_oetf(float e) {
    e = max(e, 0.0);
    if (e <= 1.0 / 12.0) return sqrt(3.0 * e);
    return 0.17883277 * log(12.0 * e - 0.28466892) + 0.55991073;
}
ER_INLINE float er_hlg_inverse_oetf(float v) {
    v = max(v, 0.0);
    if (v <= 0.5) return v * v / 3.0;
    return (exp((v - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}

/// Scene luminance of HLG 0.75 lifted by the system gamma: what SDR white (1.0 in the working space) is, so that HLG
/// 0.75 comes in at exactly 1.0 and goes out at exactly 0.75.
ER_INLINE float er_hlg_white() { return pow(er_hlg_inverse_oetf(0.75), 1.2); }

ER_INLINE float er_luma2020(vec3 c) { return 0.2627 * c.x + 0.6780 * c.y + 0.0593 * c.z; }

/// An HLG signal (Rec.2020) to the working space: scene light, the OOTF (gamma 1.2 on luminance), SDR white at 1.
ER_INLINE vec3 er_hlg_to_linear(vec3 c) {
    vec3 e = vec3(er_hlg_inverse_oetf(c.x), er_hlg_inverse_oetf(c.y), er_hlg_inverse_oetf(c.z));
    float ys = er_luma2020(e);
    if (ys <= 0.0) return vec3(0.0);
    return e * (pow(ys, 0.2) / er_hlg_white());
}

/// The working space to an HLG signal: the OOTF undone (light above HLG's peak, 1000 cd/m², is clipped), then the OETF.
ER_INLINE vec3 er_linear_to_hlg(vec3 c) {
    vec3 d = vec3(max(c.x, 0.0), max(c.y, 0.0), max(c.z, 0.0)) * er_hlg_white();
    float yd = er_luma2020(d);
    if (yd <= 0.0) return vec3(0.0);
    float ys = pow(yd, 1.0 / 1.2);
    vec3 e = d / pow(ys, 0.2);
    return vec3(er_hlg_oetf(min(e.x, 1.0)), er_hlg_oetf(min(e.y, 1.0)), er_hlg_oetf(min(e.z, 1.0)));
}

/// A PQ signal (Rec.2020, SMPTE ST 2084) to the working space: absolute light, with 203 cd/m² as SDR white.
ER_INLINE float er_pq_to_nits(float v) {
    float p = pow(max(v, 0.0), 1.0 / 78.84375);
    float d = p - 0.8359375;
    float l = max(d, 0.0) / (18.8515625 - 18.6875 * p);
    return pow(l, 1.0 / 0.1593017578125) * 10000.0;
}
ER_INLINE vec3 er_pq_to_linear(vec3 c) { return vec3(er_pq_to_nits(c.x), er_pq_to_nits(c.y), er_pq_to_nits(c.z)) / 203.0; }

/// A layer's colour (premultiplied, as it's encoded) into the working space (premultiplied, linear Rec.2020).
ER_INLINE vec4 er_to_working(vec4 c, int transfer) {
    if (c.w <= 0.0) return vec4(0.0);
    vec3 v = er_unpremultiply(c);
    vec3 l;
    if (transfer == ER_TF_HLG) l = er_hlg_to_linear(v);
    else if (transfer == ER_TF_PQ) l = er_pq_to_linear(v);
    else if (transfer == ER_TF_SRGB) l = er_709_to_2020(vec3(er_srgb_to_linear(v.x), er_srgb_to_linear(v.y), er_srgb_to_linear(v.z)));
    else l = er_709_to_2020(vec3(er_709_to_linear(v.x), er_709_to_linear(v.y), er_709_to_linear(v.z)));
    return vec4(l * c.w, c.w);
}

/// The working space to what's written out: HLG for the export, or Rec.709 for a screen without HDR (SDR white
/// stays white; brighter highlights are clipped at it, as on a display with no headroom).
ER_INLINE vec4 er_from_working(vec4 c, int hdr) {
    vec3 v = er_unpremultiply(c);
    if (hdr == ER_HDR_HLG) return vec4(er_linear_to_hlg(v) * c.w, c.w);
    vec3 s = er_2020_to_709(v);
    s = vec3(er_clamp01(s.x), er_clamp01(s.y), er_clamp01(s.z));
    return vec4(vec3(er_linear_to_709(s.x), er_linear_to_709(s.y), er_linear_to_709(s.z)) * c.w, c.w);
}

#ifdef __cplusplus
} // namespace er::cm
#endif

#endif // ER_COLORMATH_H
