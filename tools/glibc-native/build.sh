#!/usr/bin/env bash
# Run from any directory; Docker produces the shipped glibc arm64 hook and a host test build.
set -euo pipefail
cd "$(dirname "$0")/../.."
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }
MSYS_NO_PATHCONV=1 docker run --rm --platform linux/amd64 -v "$(to_host "$PWD"):/repo" debian:trixie bash -c '
set -euo pipefail
apt-get update -qq
apt-get install -y -qq --no-install-recommends gcc gcc-aarch64-linux-gnu libc6-dev libc6-dev-arm64-cross binutils-aarch64-linux-gnu
cd /repo
out=app/android/app/src/main/assets/droiddesk/glibc-native
mkdir -p "$out" tools/glibc-native/build
aarch64-linux-gnu-gcc -shared -fPIC -O2 -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 \
  -o "$out/pathhook.so" tools/glibc-native/pathhook.c -ldl
aarch64-linux-gnu-strip --strip-unneeded "$out/pathhook.so"
gcc -shared -fPIC -O2 -Wall -Wextra -Werror -o tools/glibc-native/build/pathhook-x86_64.so tools/glibc-native/pathhook.c -ldl
'
