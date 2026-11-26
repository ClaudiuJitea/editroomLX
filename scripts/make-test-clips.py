#!/usr/bin/env python3
"""Test clips whose colours are known exactly: the port of the Mac's scripts/make-test-clip.swift.

Six flat patches (grey, white, red, green, blue, skin) plus a small moving square, encoded with the exact
Y'CbCr codes for the chosen curve, then muxed by the system ffmpeg with the matching colour tags.

    scripts/make-test-clips.py <out> <sdr|hlg|pq|applelog> <hevc8|hevc10|h264> <w> <h> <fps> <seconds> [--rotate DEG]
    scripts/make-test-clips.py --set <dir>     # the standard set used by the tests and the benchmarks

Apple Log can't be tagged by ffmpeg the way AVFoundation tags it (a LogTransferFunction extension in the
sample description); the clip carries the curve and an "com.apple.quicktime.log-transfer-function" tag
instead, which is what LX reads (docs/PARITY.md).
"""
import math
import os
import subprocess
import sys

import numpy as np

PATCHES = [(118, 118, 118), (235, 235, 235), (200, 40, 40), (40, 170, 60), (40, 60, 200), (224, 172, 105)]
M709_TO_2020 = [[0.6274, 0.3293, 0.0433], [0.0691, 0.9195, 0.0114], [0.0164, 0.0880, 0.8956]]


def srgb_to_linear(v):
    c = v / 255
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def oetf(curve, e):
    if curve == "hlg":
        x = max(0, e) * 0.2647  # SDR white sits at 75% HLG (BT.2408)
        return math.sqrt(3 * x) if x <= 1 / 12 else 0.17883277 * math.log(12 * x - 0.28466892) + 0.55991073
    if curve == "pq":
        y = max(0, e) * 203 / 10000
        m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
        p = y ** m1
        return ((c1 + c2 * p) / (1 + c3 * p)) ** m2
    if curve == "applelog":
        R0, Rt, c, b, g, be = -0.05641088, 0.01, 47.28711236, 0.00964052, 0.08550479, 0.69336945
        if e >= Rt:
            return g * math.log2(e + b) + be
        if e >= R0:
            return c * (e - R0) * (e - R0)
        return 0
    x = max(0, e)
    return 4.5 * x if x < 0.018 else 1.099 * x ** 0.45 - 0.099


def ycc(rgb, curve, ten):
    wide = curve != "sdr"
    kr, kb = (0.2627, 0.0593) if wide else (0.2126, 0.0722)
    lin = [srgb_to_linear(v) for v in rgb]
    if wide:
        lin = [sum(M709_TO_2020[i][j] * lin[j] for j in range(3)) for i in range(3)]
    r, g, b = (oetf(curve, v) for v in lin)
    y = kr * r + (1 - kr - kb) * g + kb * b
    cb, cr = (b - y) / (2 * (1 - kb)), (r - y) / (2 * (1 - kr))
    s = 4.0 if ten else 1.0
    return (round(16 * s + 219 * s * y), round(128 * s + 224 * s * cb), round(128 * s + 224 * s * cr))


def make(out, curve, codec, W, H, fps, secs, rotate=0):
    ten = codec not in ("hevc8", "h264")
    codes = [ycc(p, curve, ten) for p in PATCHES]
    white = ycc((255, 255, 255), curve, ten)
    dtype = np.uint16 if ten else np.uint8
    shift = 6 if ten else 0
    cols = np.minimum(5, np.arange(W) * 6 // W)
    base_y = np.array([c[0] for c in codes])[cols]
    base_cb = np.array([c[1] for c in codes])[cols[::2]]
    base_cr = np.array([c[2] for c in codes])[cols[::2]]
    wide = curve != "sdr"
    trc = {"hlg": "arib-std-b67", "pq": "smpte2084"}.get(curve, "bt709")
    pix = "p010le" if ten else "nv12"
    enc = {"h264": ["-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p"],
           "hevc8": ["-c:v", "libx265", "-x265-params", "lossless=1:log-level=error", "-pix_fmt", "yuv420p"],
           "hevc10": ["-c:v", "libx265", "-x265-params", "lossless=1:log-level=error", "-pix_fmt", "yuv420p10le"]}[codec]
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", pix, "-s", f"{W}x{H}", "-r", str(fps), "-i", "-"]
    if rotate:
        cmd = cmd[:4] + ["-noautorotate", "-display_rotation", str(rotate)] + cmd[4:]
    prim, space = ("bt2020", "bt2020nc") if wide else ("bt709", "bt709")
    # Tag the frames themselves (setparams), so ffmpeg never converts them: the codes go in as written.
    cmd += ["-vf", f"setparams=color_primaries={prim}:color_trc={trc}:colorspace={space}:range=tv"]
    cmd += enc + ["-color_primaries", prim, "-color_trc", trc, "-colorspace", space, "-color_range", "tv"]
    if curve == "applelog":
        cmd += ["-metadata", "com.apple.quicktime.log-transfer-function=com.apple.rec2020.apple-log"]
    cmd += ["-movflags", "+use_metadata_tags", out]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    sq = H // 8
    for f in range(int(secs * fps)):
        y = np.tile(base_y, (H, 1)).astype(np.int32)
        cb = np.tile(base_cb, (H // 2, 1)).astype(np.int32)
        cr = np.tile(base_cr, (H // 2, 1)).astype(np.int32)
        sx, sy = (f * 7) % max(1, W - sq), H - sq - 4
        y[sy:sy + sq, sx:sx + sq] = white[0]
        # The Mac samples chroma at even luma positions: the same test (x*2, y*2) in the square.
        xs = np.arange(W // 2) * 2
        ys = np.arange(H // 2) * 2
        inside = (xs[None, :] >= sx) & (xs[None, :] < sx + sq) & (ys[:, None] >= sy) & (ys[:, None] < sy + sq)
        cb[inside] = white[1]
        cr[inside] = white[2]
        uv = np.empty((H // 2, W), dtype=np.int32)
        uv[:, 0::2] = cb
        uv[:, 1::2] = cr
        frame = np.concatenate([(y << shift).astype(dtype).ravel(), (uv << shift).astype(dtype).ravel()])
        p.stdin.write(frame.tobytes())
    p.stdin.close()
    p.wait()
    print(("wrote " if p.returncode == 0 else "FAILED ") + os.path.basename(out))
    return p.returncode == 0


def standard_set(d):
    os.makedirs(d, exist_ok=True)
    ok = True
    ok &= make(f"{d}/sdr_h264.mp4", "sdr", "h264", 640, 360, 30, 3)
    ok &= make(f"{d}/hlg_hevc10.mov", "hlg", "hevc10", 640, 360, 30, 2)
    ok &= make(f"{d}/pq_hevc10.mov", "pq", "hevc10", 640, 360, 30, 2)
    ok &= make(f"{d}/applelog_hevc10.mov", "applelog", "hevc10", 640, 360, 30, 2)
    ok &= make(f"{d}/rotated_h264.mp4", "sdr", "h264", 320, 240, 30, 2, rotate=-90)
    # Sound: a 440 Hz tone, and a video with sound (the SelfTest's a_main.mp4 / b_tone.wav).
    run = lambda *a: subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *a], check=True)
    run("-f", "lavfi", "-i", "sine=frequency=440:duration=4:sample_rate=48000", "-ac", "2", f"{d}/b_tone.wav")
    run("-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=4", "-f", "lavfi", "-i", "sine=frequency=660:duration=4",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "30", "-c:a", "aac", "-shortest", f"{d}/a_main.mp4")
    run("-f", "lavfi", "-i", "testsrc2=size=600x400:duration=1", "-frames:v", "1", f"{d}/photo.jpg")
    return ok


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "--set":
        sys.exit(0 if standard_set(a[1]) else 1)
    rotate = 0
    if "--rotate" in a:
        i = a.index("--rotate")
        rotate = int(a[i + 1])
        del a[i:i + 2]
    sys.exit(0 if make(a[0], a[1], a[2], int(a[3]), int(a[4]), int(a[5]), float(a[6]), rotate) else 1)
