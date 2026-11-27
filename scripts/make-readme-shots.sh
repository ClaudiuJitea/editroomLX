#!/usr/bin/env bash
# The README's screenshots: four shots cut from two screen recordings (or, without them, ffmpeg pattern footage),
# then the editor staged and captured offscreen in the Grey theme by tests/docs/tst_readme.cpp, into docs/images/.
#   scripts/make-readme-shots.sh [recording-1.mp4 recording-2.mp4]
#   (needs a debug build: cmake --preset debug && cmake --build --preset debug; delete build/readme-media to recut)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
media="$here/build/readme-media"
mkdir -p "$media" "$here/docs/images"
make() { [ -s "$media/$1" ] || ffmpeg -v error -y "${@:2}" "$media/$1"; }
if [ $# -ge 2 ]; then
  # Cut the four shots from two screen recordings, filled to 16:9, with a quiet tone for their sound.
  cut() { [ -s "$media/$1" ] || ffmpeg -v error -y -ss "$3" -t 8 -i "$2" -f lavfi -i "sine=f=$4:d=8" \
    -vf "scale=1920:1080:force_original_aspect_ratio=increase,crop=1920:1080,fps=30" -map 0:v -map 1:a \
    -c:v libx264 -crf 20 -pix_fmt yuv420p -c:a aac -shortest "$media/$1"; }
  cut Desert.mp4 "$1" 22 330; cut Highway.mp4 "$1" 58 440; cut Trail.mp4 "$2" 12 550; cut Jump.mp4 "$2" 86 262
else
  # Or, with no recordings given, demo footage from ffmpeg's own pattern sources.
  make Desert.mp4 -f lavfi -i "gradients=s=1920x1080:d=8:speed=0.02:c0=0x2a1446:c1=0xff7a3d:c2=0xffd27a:c3=0x3a6ea5:n=4" -f lavfi -i "sine=f=330:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
  make Highway.mp4 -f lavfi -i "mandelbrot=s=640x360:r=30:end_scale=0.6:end_pts=8,scale=1920:1080" -t 8 -f lavfi -i "sine=f=440:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
  make Trail.mp4 -f lavfi -i "testsrc2=s=1920x1080:r=30:d=8" -f lavfi -i "sine=f=550:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
  make Jump.mp4 -f lavfi -i "gradients=s=1920x1080:d=8:speed=0.03:c0=0x0b3d5c:c1=0x1aa3a3:c2=0x9ee6d8:n=3" -f lavfi -i "sine=f=262:d=8" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
fi
make Soundtrack.wav -f lavfi -i "aevalsrc=0.25*sin(2*PI*220*t)*(0.6+0.4*sin(2*PI*2*t))+0.15*sin(2*PI*330*t):s=48000:d=30"
cmake --build --preset debug --target tst_readme >/dev/null
ER_README_MEDIA="$media" ER_SHOTS="$here/docs/images" QT_QPA_PLATFORM=offscreen EDITROOM_AUDIO=none \
  "$here/build/debug/tests/tst_readme"
ls "$here/docs/images"
