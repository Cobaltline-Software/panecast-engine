#!/usr/bin/env bash
# Builds PanecastEngine.exe (GPL-3.0) on Windows with MSYS2 UCRT64.
#
#   C:\msys64\usr\bin\bash.exe -lc "MSYSTEM=UCRT64 ./panecast/build-windows.sh [ffmpeg-src-dir]"
#
# Needs: pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,openssl,libplist} make nasm diffutils
# Output: $BUILD_ROOT/out/PanecastEngine.exe plus the MinGW runtime DLLs it needs.
# FFmpeg is built minimal and static (only the AAC and ALAC audio decoders and libswresample).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
FFSRC="${1:-$SRC/../ffmpeg-src}"
BUILD_ROOT="${BUILD_ROOT:-$(cygpath -u "$LOCALAPPDATA")/PanecastBuild}"
JOBS="$(nproc)"
mkdir -p "$BUILD_ROOT"

# 1. Minimal static FFmpeg (LGPL-2.1+ code only; no --enable-gpl needed).
FFOUT="$BUILD_ROOT/ffmpeg-install"
if [ ! -f "$FFOUT/lib/libavcodec.a" ]; then
  mkdir -p "$BUILD_ROOT/ffmpeg-build"
  cd "$BUILD_ROOT/ffmpeg-build"
  "$FFSRC/configure" --prefix="$FFOUT" --target-os=mingw32 --arch=x86_64 \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-debug --enable-static --disable-shared \
    --disable-avdevice --disable-avformat --disable-swscale --disable-avfilter \
    --enable-swresample --enable-decoder=aac,alac --enable-parser=aac
  make -j"$JOBS"
  make install
fi

# 2. The engine itself.
cd "$BUILD_ROOT"
cmake -S "$HERE" -B "$BUILD_ROOT/engine-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNO_MARCH_NATIVE=ON -DFFMPEG_PREFIX="$FFOUT"
cmake --build "$BUILD_ROOT/engine-build" -j "$JOBS"

# 3. Collect the exe and the DLLs it imports from the UCRT64 tree.
OUT="$BUILD_ROOT/out"
rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BUILD_ROOT/engine-build/PanecastEngine.exe" "$OUT/"
strip "$OUT/PanecastEngine.exe"
for dll in $(ldd "$OUT/PanecastEngine.exe" | awk '/ucrt64/ {print $3}' | sort -u); do
  cp "$dll" "$OUT/"
done
ls -la "$OUT"
