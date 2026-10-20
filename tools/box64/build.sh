#!/usr/bin/env bash
# Builds Box64 for the Debian (trixie, arm64) container and packs it as
# box64-<version>-<revision>-arm64.tar.xz (unpacks to /opt/droiddesk-wine/box64).
# Runs in an emulated arm64 Docker container; enable emulation once with
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
# Run from the repo root. See docs/windows-apps-plan.md (Artifacts).
#
# Layout of the tarball (Box64 installs /etc and /usr/lib/box64-* with absolute paths, so the
# staged files are moved under the prefix; the droiddesk-wine wrapper points Box64 at them):
#   bin/box64                         BOX64_RCFILE=/opt/droiddesk-wine/box64/etc/box64.box64rc
#   etc/box64.box64rc                 BOX64_LD_LIBRARY_PATH=/opt/droiddesk-wine/box64/lib/x86_64-linux-gnu
#   lib/x86_64-linux-gnu/             x86_64 libstdc++/libgcc_s/... shipped by Box64
#   share/doc/box64/LICENSE           MIT
#   .droiddesk-version
# No binfmt config: proot has no binfmt, the wrapper calls box64 explicitly.
set -euo pipefail

BOX64_VERSION=0.4.4
# Bump when the build changes for the same Box64 version (asset name and tag).
ASSET_REVISION=1
# Commit of tag v0.4.4 (github.com/ptitSeb/box64); the clone is checked against it.
BOX64_COMMIT=2f130fab1d6e1a4ee8a71dc60cfdfcc839ad192a
PREFIX=/opt/droiddesk-wine/box64
ASSET=box64-$BOX64_VERSION-$ASSET_REVISION-arm64.tar.xz
OUT_DIR="${OUT_DIR:-$HOME/DroidDesk-build}"
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }

mkdir -p "$OUT_DIR"
MSYS_NO_PATHCONV=1 docker run --rm --platform linux/arm64 \
    -v "$(to_host "$OUT_DIR"):/out" debian:trixie bash -c "
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates git cmake make gcc g++ python3 \
    xz-utils >/dev/null

cd /tmp
git clone -q --depth 1 --branch v$BOX64_VERSION https://github.com/ptitSeb/box64.git
cd box64
[ \"\$(git rev-parse HEAD)\" = $BOX64_COMMIT ] || { echo 'box64 tag moved' >&2; exit 1; }

cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DARM64=1 -DBOX32=ON -DBAD_SIGNAL=ON -DARM_DYNAREC=ON
make -C build -j\$(nproc)
DESTDIR=/tmp/stage make -C build install

cd /tmp/stage
mkdir -p .$PREFIX/etc .$PREFIX/lib .$PREFIX/share/doc/box64
mv etc/box64.box64rc .$PREFIX/etc/
mv usr/lib/box64-x86_64-linux-gnu .$PREFIX/lib/x86_64-linux-gnu
rm -rf etc usr
cp /tmp/box64/LICENSE .$PREFIX/share/doc/box64/LICENSE
echo $BOX64_VERSION-$ASSET_REVISION > .$PREFIX/.droiddesk-version
./${PREFIX#/}/bin/box64 --version || true
tar -C /tmp/stage -cJf /out/$ASSET ${PREFIX#/}
sha256sum /out/$ASSET
"
