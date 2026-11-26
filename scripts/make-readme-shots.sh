#!/usr/bin/env bash
# The README's screenshots: demo footage made with ffmpeg's own pattern sources (nothing third-party), then the
# editor staged and captured offscreen in the Grey theme by tests/docs/tst_readme.cpp, into docs/images/.
#   scripts/make-readme-shots.sh            (needs a debug build: cmake --preset debug && cmake --build --preset debug)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
media="$here/build/readme-media"
mkdir -p "$media" "$here/docs/images"
make() { [ -s "$media/$1" ] || ffmpeg -v error -y "${@:2}" "$media/$1"; }
make Sunrise.mp4 -f lavfi -i "gradients=s=1920x1080:d=8:speed=0.02:c0=0x2a1446:c1=0xff7a3d:c2=0xffd27a:c3=0x3a6ea5:n=4" -f lavfi -i "sine=f=330:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
make Fractal.mp4 -f lavfi -i "mandelbrot=s=640x360:r=30:end_scale=0.6:end_pts=8,scale=1920:1080" -t 8 -f lavfi -i "sine=f=440:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
make Studio.mp4 -f lavfi -i "testsrc2=s=1920x1080:r=30:d=8" -f lavfi -i "sine=f=550:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
make Ocean.mp4 -f lavfi -i "gradients=s=1920x1080:d=8:speed=0.03:c0=0x0b3d5c:c1=0x1aa3a3:c2=0x9ee6d8:n=3" -f lavfi -i "sine=f=262:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
make Soundtrack.wav -f lavfi -i "aevalsrc=0.25*sin(2*PI*220*t)*(0.6+0.4*sin(2*PI*2*t))+0.15*sin(2*PI*330*t):s=48000:d=30"
cmake --build --preset debug --target tst_readme >/dev/null
ER_README_MEDIA="$media" ER_SHOTS="$here/docs/images" QT_QPA_PLATFORM=offscreen EDITROOM_AUDIO=none \
  "$here/build/debug/tests/tst_readme"
ls "$here/docs/images"
