#!/usr/bin/env bash
# Builds Editroom-LX-x86_64.AppImage from a release build, with linuxdeploy and its Qt plugin.
#
#   scripts/make-appimage.sh <folder with linuxdeploy-x86_64.AppImage and linuxdeploy-plugin-qt-x86_64.AppImage>
#
# Get the tools from https://github.com/linuxdeploy/linuxdeploy/releases (continuous) and
# https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases (continuous).
#
# FFmpeg is NOT bundled: distribution FFmpeg builds are often configured with --enable-gpl, and Editroom ships no
# GPL code without the owner's agreement (PROMPT.md). The AppImage uses the system's FFmpeg 8 (libavcodec62),
# Vulkan loader, VA-API and graphics drivers; ONNX Runtime stays optional (loaded when scene tags are used).
set -euo pipefail
tools=${1:?folder with the linuxdeploy AppImages}
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
cmake --preset release
cmake --build --preset release
rm -rf build/AppDir
DESTDIR="$here/build/AppDir" cmake --install build/release --prefix /usr
chmod +x "$tools"/linuxdeploy*.AppImage
ln -sf "$tools/linuxdeploy-plugin-qt-x86_64.AppImage" "$tools/linuxdeploy-plugin-qt"
export PATH="$tools:$PATH" QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)} APPIMAGE_EXTRACT_AND_RUN=1
export OUTPUT=Editroom-LX-x86_64.AppImage
# Wayland as well as X11 (the viewer presents natively on both), and offscreen for headless checks. The Qt plugin
# then brings the Wayland shell, decoration and graphics integration plugins along.
export EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
export EXTRA_QT_MODULES="waylandclient"
cd build
"$tools/linuxdeploy-x86_64.AppImage" --appdir AppDir \
  -e AppDir/usr/bin/editroom-lx \
  -d AppDir/usr/share/applications/editroom.desktop \
  -i AppDir/usr/share/icons/hicolor/256x256/apps/editroom.png \
  --plugin qt \
  --exclude-library 'libavcodec*' --exclude-library 'libavformat*' --exclude-library 'libavutil*' \
  --exclude-library 'libavfilter*' --exclude-library 'libavdevice*' --exclude-library 'libswscale*' \
  --exclude-library 'libswresample*' --exclude-library 'libpostproc*' --exclude-library 'libx264*' \
  --exclude-library 'libx265*' --exclude-library 'libvulkan*' --exclude-library 'libonnxruntime*' \
  --exclude-library 'libva*' --exclude-library 'libdrm*' --exclude-library 'libGL*' --exclude-library 'libEGL*' \
  --output appimage
echo "Built build/$OUTPUT"
