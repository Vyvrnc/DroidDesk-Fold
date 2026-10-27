#!/usr/bin/env bash
# Rebuilds jniLibs/arm64-v8a/libdroiddesk_v4l2.so (fake /dev/video0 for Termux Firefox,
# see docs/camera-bridge-research.md "Prototype status") from tools/v4l2-shim/ with
# NDK r28c, API 29, like build-socket-hook.sh. Also builds the bionic test programs
# tools/v4l2-shim/out/v4l2_capture_test and out/fake_camera_bridge (not committed).
# Run from the repo root.
# Commit the source and the binary together.
set -euo pipefail

IMAGE=ghcr.io/cirruslabs/flutter:stable
NDK=28.2.13676358
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }
mkdir -p tools/v4l2-shim/out

MSYS_NO_PATHCONV=1 docker run --rm -v "$(to_host "$PWD"):/repo" -v droiddesk-ndk:/opt/android-sdk-linux/ndk "$IMAGE" bash -c "
set -e
yes | sdkmanager --licenses >/dev/null 2>&1 || true
[ -d /opt/android-sdk-linux/ndk/$NDK ] || sdkmanager 'ndk;$NDK' >/dev/null
T=/opt/android-sdk-linux/ndk/$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
cd /repo/tools/v4l2-shim
\$T/aarch64-linux-android29-clang -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wl,-z,max-page-size=16384 \
  -o /repo/app/android/app/src/main/jniLibs/arm64-v8a/libdroiddesk_v4l2.so \
  v4l2_shim.c frame_source_bridge.c frame_source_pattern.c -ldl
\$T/aarch64-linux-android29-clang -O2 -Wall -o out/v4l2_capture_test v4l2_capture_test.c
\$T/aarch64-linux-android29-clang -O2 -Wall -Wextra -Wno-unused-parameter -o out/fake_camera_bridge \
  fake_camera_bridge.c frame_source_pattern.c
\$T/llvm-strip --strip-unneeded /repo/app/android/app/src/main/jniLibs/arm64-v8a/libdroiddesk_v4l2.so
\$T/llvm-nm -D --defined-only /repo/app/android/app/src/main/jniLibs/arm64-v8a/libdroiddesk_v4l2.so \
  | grep -w -E 'open|openat|close|ioctl|mmap|mmap64|dup|fcntl|stat|fstat'
if \$T/llvm-nm -D --defined-only /repo/app/android/app/src/main/jniLibs/arm64-v8a/libdroiddesk_v4l2.so | grep -w dd_log; then
  echo 'dd_log must stay hidden'; exit 1
fi
"
