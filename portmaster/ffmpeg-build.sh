#!/bin/bash
# Build a minimal, decode-only FFmpeg (shared libs) for the ct_pm PortMaster port.
# Runs inside the PortMaster aarch64 builder (glibc 2.31, native arm64 under colima).
# Only what the cutscenes need: MP4(mov) demux + H.264 & AAC decode + swscale/swresample.
# Output goes to /out (mounted to ~/Desktop/ct_switch_build/ffmpeg-aarch64 on the host).
set -e

SRC=/work/ffmpeg-7.1.1
OUT=/out

if [ ! -d "$SRC" ]; then
  echo "== extracting source =="
  cd /work && tar xf ffmpeg-7.1.1.tar.xz
fi

cd "$SRC"

echo "== configure =="
./configure \
  --prefix="$OUT" \
  --enable-shared --disable-static \
  --enable-pic \
  --disable-programs --disable-doc \
  --disable-everything \
  --disable-network --disable-autodetect --disable-debug \
  --disable-x86asm \
  --arch=aarch64 --target-os=linux \
  --enable-swscale --enable-swresample --enable-avformat --enable-avcodec \
  --enable-demuxer=mov,m4v,h264,aac \
  --enable-decoder=h264,hevc,aac,aac_latm \
  --enable-parser=h264,hevc,aac,aac_latm \
  --enable-bsf=h264_mp4toannexb,aac_adtstoasc,extract_extradata \
  --enable-protocol=file

echo "== build =="
make -j"$(nproc)"
echo "== install =="
make install

echo "== strip + report =="
strip --strip-unneeded "$OUT"/lib/lib*.so.* 2>/dev/null || true
ls -la "$OUT"/lib/*.so*
echo "TOTAL bundle size:"
du -ch "$OUT"/lib/*.so.*[0-9] 2>/dev/null | tail -1 || du -ch "$OUT"/lib/*.so* | tail -1
echo "== sonames (NEEDED on device) =="
for f in "$OUT"/lib/lib*.so; do readlink -f "$f"; done
echo "BUILD_OK"
