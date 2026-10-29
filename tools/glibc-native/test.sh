#!/usr/bin/env bash
# Optional: DD_TEST_PLATFORM=linux/arm64 ./tools/glibc-native/test.sh (requires Docker binfmt/QEMU).
set -euo pipefail
cd "$(dirname "$0")/../.."
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }
MSYS_NO_PATHCONV=1 docker run --rm --platform "${DD_TEST_PLATFORM:-linux/amd64}" \
  -v "$(to_host "$PWD"):/repo:ro" debian:trixie bash /repo/tools/glibc-native/test-container.sh
