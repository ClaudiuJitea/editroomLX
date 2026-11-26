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
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

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
    int pad0;
    int pad1;
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
    int pad0;
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

#ifdef __cplusplus
} // namespace er::cm
#endif

#endif // ER_COLORMATH_H
