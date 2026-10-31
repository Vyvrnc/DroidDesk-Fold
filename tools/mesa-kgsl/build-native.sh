#!/usr/bin/env bash
# Builds the same Mesa as build.sh, natively in DroidDesk's Debian on the phone (arm64, about
# 35 min with -j3 on the Fold 7). Needs the build dependencies from build.sh (apt, under proot).
# Usage: bash build-native.sh [PATCH_DIR]   PATCH_DIR holds our *.patch files (default: next to this script)
# Output: ~/mesa-kgsl-VERSION-REVISION-arm64.tar.xz and its sha256.
set -euo pipefail

MESA_VERSION=26.2.6
ASSET_REVISION=1
MESA_SHA256=b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92
TERMUX_PACKAGES_COMMIT=84f9775ddbeda5eb04ceabd7c86f340ac4c9ccbb
PATCH_DIR=${1:-$(cd "$(dirname "$0")" && pwd)}
WORK=${WORK:-$HOME/mesa-build}
JOBS=${JOBS:-3}

mkdir -p "$WORK" && cd "$WORK"
[ -f mesa-$MESA_VERSION.tar.xz ] || curl -fsSLo mesa-$MESA_VERSION.tar.xz https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz
echo "$MESA_SHA256  mesa-$MESA_VERSION.tar.xz" | sha256sum -c -
rm -rf mesa-$MESA_VERSION stage
tar xf mesa-$MESA_VERSION.tar.xz
cd mesa-$MESA_VERSION
for p in 0014-replace-turnip-wait_timestamp_safe-assert.patch \
         0017-preserve-egl-support-in-zink.patch \
         0018-disable-general-layout-in-zink-for-turnip.patch; do
    curl -fsSL https://raw.githubusercontent.com/termux/termux-packages/$TERMUX_PACKAGES_COMMIT/packages/mesa/$p | patch -p1
done
for p in "$PATCH_DIR"/*.patch; do patch -p1 < "$p"; done

meson setup build --prefix=/opt/mesa-kgsl --libdir=lib/aarch64-linux-gnu -Dbuildtype=release \
    -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm,kgsl -Dgallium-drivers=zink,softpipe -Dplatforms=x11 \
    -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dglvnd=enabled \
    -Dllvm=disabled -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dvideo-codecs= -Dbuild-tests=false
ninja -C build -j"$JOBS"
DESTDIR="$WORK/stage" ninja -C build install
echo "$MESA_VERSION-$ASSET_REVISION" > "$WORK/stage/opt/mesa-kgsl/.droiddesk-version"
OUT=$HOME/mesa-kgsl-$MESA_VERSION-$ASSET_REVISION-arm64.tar.xz
tar -C "$WORK/stage" -cJf "$OUT" opt/mesa-kgsl
sha256sum "$OUT"
