#!/usr/bin/env bash
# Rebuilds jniLibs/arm64-v8a/libsocket_hook.so from app/assets/socket_hook.c
# (NDK r28c, API 29, same as the shipped binary). Run from the repo root.
# Commit the source and the binary together.
set -euo pipefail

IMAGE=ghcr.io/cirruslabs/flutter:stable
NDK=28.2.13676358
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }

MSYS_NO_PATHCONV=1 docker run --rm -v "$(to_host "$PWD"):/repo" -v droiddesk-ndk:/opt/android-sdk-linux/ndk "$IMAGE" bash -c "
set -e
yes | sdkmanager --licenses >/dev/null 2>&1 || true
[ -d /opt/android-sdk-linux/ndk/$NDK ] || sdkmanager 'ndk;$NDK' >/dev/null
T=/opt/android-sdk-linux/ndk/$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
cd /repo/app
\$T/aarch64-linux-android29-clang -shared -fPIC -O2 -Wall \
  -o android/app/src/main/jniLibs/arm64-v8a/libsocket_hook.so assets/socket_hook.c -ldl -llog
\$T/llvm-nm -D --defined-only android/app/src/main/jniLibs/arm64-v8a/libsocket_hook.so | grep -w -E 'bind|connect|open|execve|execvp'
"
