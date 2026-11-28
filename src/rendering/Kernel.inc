// Kernel.inc — one output pixel of the timeline compositor (TimelineCompositor.compose), shared word for
// word by the Vulkan shader (shaders/rendering.comp) and the CPU renderer (CpuRenderer.cpp).
//
// The host defines, before including this file:
//   ErFrame er_frame();                 ErLayer er_layer(int i);        ErSource er_source(int i);
//   int er_byte(ErSource s, int byteOffset);   int er_u16(ErSource s, int byteOffset);   float er_cubef(int index);
//   ivec2 er_u16x2(ErSource s, int byteOffset);   (two little-endian u16s: the shader reads them as one word)
// (Offsets are within the source; the source's `pad0` is its index, for hosts that keep sources apart.)
// Coordinates are y-down, in pixels, with pixel centres at +0.5. Core Image is y-up; every y term was
// turned around when the frame was described (RenderPlan), so the maths here has no y flips.

/// One stored texel as premultiplied RGBA: YUV formats converted with their matrix and range (chroma
/// interpolated bilinearly from its half-size plane, edges clamped), RGBA8 as stored.
ER_INLINE vec4 er_texel(ErSource s, int x, int y) {
    if (s.format == ER_SRC_RGBA8) {
        ivec2 w = er_u16x2(s, s.offset0 + y * s.stride0 + x * 4);
        return vec4(float(w.x & 255), float(w.x >> 8), float(w.y & 255), float(w.y >> 8)) / 255.0;
    }
    bool wide = s.format == ER_SRC_P010 || s.format == ER_SRC_YUV420P10;
    float maxCode = wide ? 1023.0 : 255.0;
    float yc;
    if (s.format == ER_SRC_P010) yc = float(er_u16(s, s.offset0 + y * s.stride0 + x * 2) >> 6);
    else if (s.format == ER_SRC_YUV420P10) yc = float(er_u16(s, s.offset0 + y * s.stride0 + x * 2) & 1023);
    else yc = float(er_byte(s, s.offset0 + y * s.stride0 + x));
    // Chroma sits at the centre of each 2×2 block.
    int cw = (s.width + 1) / 2;
    int ch = (s.height + 1) / 2;
    float cx = (float(x) + 0.5) * 0.5 - 0.5;
    float cy = (float(y) + 0.5) * 0.5 - 0.5;
    float fx0 = floor(cx);
    float fy0 = floor(cy);
    float tx = cx - fx0;
    float ty = cy - fy0;
    float cb = 0.0;
    float cr = 0.0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int px = clamp(int(fx0) + i, 0, cw - 1);
            int py = clamp(int(fy0) + j, 0, ch - 1);
            float w = (i == 0 ? 1.0 - tx : tx) * (j == 0 ? 1.0 - ty : ty);
            float u;
            float v;
            if (s.format == ER_SRC_NV12) {
                int w = er_u16(s, s.offset1 + py * s.stride1 + px * 2);
                u = float(w & 255);
                v = float(w >> 8);
            } else if (s.format == ER_SRC_P010) {
                ivec2 w = er_u16x2(s, s.offset1 + py * s.stride1 + px * 4);
                u = float(w.x >> 6);
                v = float(w.y >> 6);
            } else if (s.format == ER_SRC_YUV420P10) {
                u = float(er_u16(s, s.offset1 + py * s.stride1 + px * 2) & 1023);
                v = float(er_u16(s, s.offset2 + py * s.stride1 + px * 2) & 1023);
            } else {
                u = float(er_byte(s, s.offset1 + py * s.stride1 + px));
                v = float(er_byte(s, s.offset2 + py * s.stride1 + px));
            }
            cb = cb + u * w;
            cr = cr + v * w;
        }
    }
    vec3 rgb = er_yuv_to_rgb(yc / maxCode, cb / maxCode, cr / maxCode, s.matrix, s.fullRange, maxCode);
    return vec4(rgb, 1.0);
}

/// A texel of the oriented picture; transparent outside it.
ER_INLINE vec4 er_oriented_texel(ErSource s, int u, int v) {
    int ow = er_swaps(s.orientation) ? s.height : s.width;
    int oh = er_swaps(s.orientation) ? s.width : s.height;
    if (u < 0 || v < 0 || u >= ow || v >= oh) return vec4(0.0);
    ivec2 p = er_orient(u, v, s.orientation, s.width, s.height);
    return er_texel(s, p.x, p.y);
}

/// Bilinear sample of the oriented picture at (x, y), transparent outside (Core Image's affine
/// transform of a finite image).
ER_INLINE vec4 er_sample(ErSource s, vec2 p) {
    ivec2 p0;
    vec2 f;
    er_bilinear_taps(p.x, p.y, p0, f);
    // A tap whose weight is exactly 0 adds exactly 0, so it isn't fetched (a picture placed 1:1 reads one texel,
    // not four); the result is bit for bit the same.
    vec4 a = er_oriented_texel(s, p0.x, p0.y);
    vec4 b = f.x == 0.0 ? a : er_oriented_texel(s, p0.x + 1, p0.y);
    vec4 c = f.y == 0.0 ? a : er_oriented_texel(s, p0.x, p0.y + 1);
    vec4 d = f.x == 0.0 ? c : (f.y == 0.0 ? b : er_oriented_texel(s, p0.x + 1, p0.y + 1));
    return er_bilerp(a, b, c, d, f);
}

// MARK: Upscaling (export)
//
// The Mac enlarges video drawn bigger than its own pixels with MetalFX's spatial upscaler. Linux has no MetalFX; the
// nearest open equivalent is AMD FidelityFX Super Resolution 1's EASU (Edge Adaptive Spatial Upsampling, MIT), ported
// here: 12 taps around the point, the local edge direction and strength from their luma, a Lanczos-like kernel
// stretched along the edge, and the result kept within the four nearest texels (no ringing).

/// FSR's luma stand-in: green plus half of red and blue.
ER_INLINE float er_easu_luma(vec4 c) { return c.x * 0.5 + c.z * 0.5 + c.y; }

/// One 2×2 quad's contribution to the direction and length (FsrEasuSetF).
ER_INLINE void er_easu_set(ER_INOUT(vec2) dir, ER_INOUT(float) len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB;
    dir.x = dir.x + dirX * w;
    lenX = er_clamp01(abs(dirX) * lenX);
    lenX = lenX * lenX;
    len = len + lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA;
    dir.y = dir.y + dirY * w;
    lenY = er_clamp01(abs(dirY) * lenY);
    lenY = lenY * lenY;
    len = len + lenY * w;
}

/// One tap (FsrEasuTapF): its weight from its offset, rotated to the edge and stretched along it.
ER_INLINE void er_easu_tap(ER_INOUT(vec4) aC, ER_INOUT(float) aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec4 c) {
    float vx = off.x * dir.x + off.y * dir.y;
    float vy = off.x * (-dir.y) + off.y * dir.x;
    vx = vx * len2.x;
    vy = vy * len2.y;
    float d2 = min(vx * vx + vy * vy, clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB = wB * wB;
    wA = wA * wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC = aC + c * w;
    aW = aW + w;
}

ER_INLINE vec4 er_min4(vec4 a, vec4 b) { return vec4(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z), min(a.w, b.w)); }
ER_INLINE vec4 er_max4(vec4 a, vec4 b) { return vec4(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z), max(a.w, b.w)); }

/// The oriented picture at (x, y) (pixel units, centres at +0.5), enlarged with EASU; transparent outside.
ER_INLINE vec4 er_sample_easu(ErSource s, vec2 p) {
    float px = p.x - 0.5;
    float py = p.y - 0.5;
    float fx0 = floor(px);
    float fy0 = floor(py);
    int x = int(fx0);
    int y = int(fy0);
    vec2 pp = vec2(px - fx0, py - fy0);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec4 b = er_oriented_texel(s, x, y - 1);
    vec4 c = er_oriented_texel(s, x + 1, y - 1);
    vec4 e = er_oriented_texel(s, x - 1, y);
    vec4 f = er_oriented_texel(s, x, y);
    vec4 g = er_oriented_texel(s, x + 1, y);
    vec4 h = er_oriented_texel(s, x + 2, y);
    vec4 i = er_oriented_texel(s, x - 1, y + 1);
    vec4 j = er_oriented_texel(s, x, y + 1);
    vec4 k = er_oriented_texel(s, x + 1, y + 1);
    vec4 l = er_oriented_texel(s, x + 2, y + 1);
    vec4 n = er_oriented_texel(s, x, y + 2);
    vec4 o = er_oriented_texel(s, x + 1, y + 2);
    float bL = er_easu_luma(b), cL = er_easu_luma(c), eL = er_easu_luma(e), fL = er_easu_luma(f);
    float gL = er_easu_luma(g), hL = er_easu_luma(h), iL = er_easu_luma(i), jL = er_easu_luma(j);
    float kL = er_easu_luma(k), lL = er_easu_luma(l), nL = er_easu_luma(n), oL = er_easu_luma(o);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    er_easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    er_easu_set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    er_easu_set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    er_easu_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    // Normalised direction; none (flat) reads as across.
    float dirR = dir.x * dir.x + dir.y * dir.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : 1.0 / sqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir = dir * dirR;
    len = len * 0.5;
    len = len * len;
    // Stretched along the edge, by how strong it is.
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    vec4 aC = vec4(0.0);
    float aW = 0.0;
    er_easu_tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    er_easu_tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    er_easu_tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    er_easu_tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    er_easu_tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    er_easu_tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    er_easu_tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    er_easu_tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    er_easu_tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    er_easu_tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    er_easu_tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    er_easu_tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    // Kept within the four nearest texels, so edges don't ring.
    vec4 lo = er_min4(er_min4(f, g), er_min4(j, k));
    vec4 hi = er_max4(er_max4(f, g), er_max4(j, k));
    vec4 r = aW != 0.0 ? aC / aW : f;
    return er_min4(hi, er_max4(lo, r));
}

ER_INLINE vec3 er_cube_at(int base, int n, int r, int g, int b) {
    int i = base + er_cube_index(r, g, b, n) * 4;
    return vec3(er_cubef(i), er_cubef(i + 1), er_cubef(i + 2));
}

/// CIColorCube on a premultiplied colour: unpremultiplied, looked up, premultiplied again.
ER_INLINE vec4 er_apply_cube(vec4 c, int base, int n) {
    if (base < 0 || c.w <= 0.0) return c;
    vec3 rgb = er_unpremultiply(c);
    ivec3 i0;
    vec3 f;
    er_cube_cell(rgb, n, i0, f);
    vec3 o = er_trilerp(er_cube_at(base, n, i0.x, i0.y, i0.z), er_cube_at(base, n, i0.x + 1, i0.y, i0.z),
                        er_cube_at(base, n, i0.x, i0.y + 1, i0.z), er_cube_at(base, n, i0.x + 1, i0.y + 1, i0.z),
                        er_cube_at(base, n, i0.x, i0.y, i0.z + 1), er_cube_at(base, n, i0.x + 1, i0.y, i0.z + 1),
                        er_cube_at(base, n, i0.x, i0.y + 1, i0.z + 1), er_cube_at(base, n, i0.x + 1, i0.y + 1, i0.z + 1), f);
    return vec4(o * c.w, c.w);
}

/// The grade in an HDR project: worked on the HLG signal, which runs 0…1 like the cube (highlights above standard
/// white included), then back.
ER_INLINE vec4 er_apply_grade(vec4 c, int base, int hdr) {
    if (hdr == ER_HDR_OFF || base < 0 || c.w <= 0.0) return er_apply_cube(c, base, 32);
    vec4 h = vec4(er_linear_to_hlg(er_unpremultiply(c)) * c.w, c.w);
    h = er_apply_cube(h, base, 32);
    return vec4(er_hlg_to_linear(er_unpremultiply(h)) * c.w, c.w);
}

/// One layer's premultiplied colour at canvas point P, opacity included.
ER_INLINE vec4 er_layer_color(ErLayer L, vec2 P, ErFrame F) {
    float W = float(F.canvasW);
    float H = float(F.canvasH);
    // Undo the clip's transform: scaled about the centre, then offset.
    vec2 q = P;
    if (L.scale != 1.0 || L.offsetX != 0.0 || L.offsetY != 0.0) {
        q = vec2((P.x - W * 0.5 - L.offsetX) / L.scale + W * 0.5, (P.y - H * 0.5 - L.offsetY) / L.scale + H * 0.5);
    }
    // A wipe in progress: cropped to the left part of the frame (before the transform, as on the Mac).
    if (L.wipe >= 0.0 && (q.x >= W * L.wipe || q.x < 0.0 || q.y < 0.0 || q.y >= H)) return vec4(0.0);
    vec4 c;
    if (L.kind == ER_LAYER_SOLID) {
        if (q.x < 0.0 || q.y < 0.0 || q.x >= W || q.y >= H) return vec4(0.0);
        c = vec4(L.r, L.g, L.b, L.a);
    } else if (L.kind == ER_LAYER_PIXMAP) {
        c = er_sample(er_source(L.source), vec2(q.x - float(L.pixX), q.y - float(L.pixY)));
    } else {
        ErSource s = er_source(L.source);
        vec2 sp = vec2((q.x - L.fitX) / L.fitScale, (q.y - L.fitY) / L.fitScale);
        // Video drawn bigger than its own pixels, on export: enlarged with detail rebuilt rather than stretched.
        c = L.upscale != 0 ? er_sample_easu(s, sp) : er_sample(s, sp);
        // HDR and Log to Rec.709, after scaling (SourceColor.toRec709). An HDR project's HDR clips have none: they
        // stay HLG (PQ made HLG).
        c = er_apply_cube(c, L.colorCube, 64);
    }
    // An HDR project: what the layer's values mean, into the working space (SDR at standard white).
    if (F.hdr != ER_HDR_OFF) c = er_to_working(c, L.transfer);
    c = er_apply_grade(c, L.gradeCube, F.hdr);
    return c * L.opacity;
}

/// The canvas at point P: opaque black, then every layer bottom first.
ER_INLINE vec4 er_composite(vec2 P, ErFrame F) {
    vec4 out_ = vec4(0.0, 0.0, 0.0, 1.0);
    for (int i = 0; i < F.layerCount; i++) {
        ErLayer L = er_layer(i);
        vec4 c = er_layer_color(L, P, F);
        if (c.w > 0.0 || c.x != 0.0 || c.y != 0.0 || c.z != 0.0) out_ = er_over(c, out_);
    }
    return out_;
}

/// The canvas pixel (x, y), transparent outside the canvas.
ER_INLINE vec4 er_canvas_pixel(int x, int y, ErFrame F) {
    if (x < 0 || y < 0 || x >= F.canvasW || y >= F.canvasH) return vec4(0.0);
    return er_composite(vec2(float(x) + 0.5, float(y) + 0.5), F);
}

/// One output pixel: the canvas through the output's window (resampled when the window sits between
/// pixels, as Core Image's translate does), then the captions on top, in the output's own frame.
ER_INLINE vec4 er_output_pixel(int x, int y) {
    ErFrame F = er_frame();
    vec4 c;
    float fx = F.offsetX - floor(F.offsetX);
    float fy = F.offsetY - floor(F.offsetY);
    if (fx == 0.0 && fy == 0.0) {
        c = er_canvas_pixel(x + int(F.offsetX), y + int(F.offsetY), F);
    } else {
        ivec2 p0;
        vec2 f;
        er_bilinear_taps(float(x) + 0.5 + F.offsetX, float(y) + 0.5 + F.offsetY, p0, f);
        vec4 a = er_canvas_pixel(p0.x, p0.y, F);
        vec4 b = f.x == 0.0 ? a : er_canvas_pixel(p0.x + 1, p0.y, F);
        vec4 cc = f.y == 0.0 ? a : er_canvas_pixel(p0.x, p0.y + 1, F);
        vec4 d = f.x == 0.0 ? cc : (f.y == 0.0 ? b : er_canvas_pixel(p0.x + 1, p0.y + 1, F));
        c = er_bilerp(a, b, cc, d, f);
    }
    for (int i = 0; i < F.captionCount; i++) {
        ErLayer L = er_layer(F.firstCaption + i);
        ErSource s = er_source(L.source);
        int u = x - L.pixX;
        int v = y - L.pixY;
        if (u >= 0 && v >= 0 && u < s.width && v < s.height) {
            vec4 t = er_texel(s, u, v);
            if (F.hdr != ER_HDR_OFF) t = er_to_working(t, ER_TF_SRGB);
            c = er_over(t * L.opacity, c);
        }
    }
    // An HDR project: written as HLG, or shown as Rec.709 on a screen without HDR.
    if (F.hdr != ER_HDR_OFF) c = er_from_working(c, F.hdr);
    return c;
}
