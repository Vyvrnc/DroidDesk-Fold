#!/usr/bin/env bash
# Builds Mesa with Turnip over KGSL + Zink for the Debian (trixie, arm64) container
# and packs it as mesa-kgsl-<version>-<revision>-arm64.tar.xz (unpacks to /opt/mesa-kgsl).
# Runs in an emulated arm64 Docker container; enable emulation once with
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
# Run from the repo root. Takes a few hours under emulation. See NOTES.md.
set -euo pipefail

MESA_VERSION=26.2.3
# Bump when the build changes for the same Mesa version (asset name and tag).
ASSET_REVISION=2
MESA_SHA256=1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f
TERMUX_PACKAGES_COMMIT=dab70fe87265d1e0f56375e47e27d17a1bf607b4
OUT_DIR="${OUT_DIR:-$HOME/DroidDesk-build}"
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }

mkdir -p "$OUT_DIR"
MSYS_NO_PATHCONV=1 docker run --rm --platform linux/arm64 \
    -v "$(to_host "$OUT_DIR"):/out" debian:trixie bash -c "
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl patch xz-utils meson ninja-build \
    python3-mako python3-yaml python3-packaging glslang-tools bison flex libdrm-dev libx11-dev libxext-dev \
    libxfixes-dev libxcb1-dev libxcb-dri2-0-dev libxcb-dri3-dev libxcb-present-dev libxcb-glx0-dev \
    libxcb-shm0-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-randr0-dev libxcb-keysyms1-dev libx11-xcb-dev \
    libxshmfence-dev libxxf86vm-dev libxrandr-dev libexpat1-dev zlib1g-dev libzstd-dev pkg-config cmake \
    libvulkan-dev libglvnd-dev x11proto-dev gcc g++ spirv-tools libelf-dev libarchive-dev libxml2-dev >/dev/null

cd /tmp
curl -fsSLo mesa.tar.xz https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz
echo '$MESA_SHA256  mesa.tar.xz' | sha256sum -c -
tar xf mesa.tar.xz
cd mesa-$MESA_VERSION
# Only the patches that matter outside bionic (NOTES.md).
for p in 0014-replace-turnip-wait_timestamp_safe-assert.patch \
         0017-preserve-egl-support-in-zink.patch \
         0018-disable-general-layout-in-zink-for-turnip.patch; do
    curl -fsSL https://raw.githubusercontent.com/termux/termux-packages/$TERMUX_PACKAGES_COMMIT/packages/mesa/\$p | patch -p1
done

meson setup build --prefix=/opt/mesa-kgsl --libdir=lib/aarch64-linux-gnu -Dbuildtype=release \
    -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm,kgsl -Dgallium-drivers=zink,softpipe -Dplatforms=x11 \
    -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dglvnd=enabled \
    -Dllvm=disabled -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dvideo-codecs= -Dbuild-tests=false
ninja -C build
DESTDIR=/tmp/stage ninja -C build install
tar -C /tmp/stage -cJf /out/mesa-kgsl-$MESA_VERSION-$ASSET_REVISION-arm64.tar.xz opt/mesa-kgsl
sha256sum /out/mesa-kgsl-$MESA_VERSION-$ASSET_REVISION-arm64.tar.xz
"
