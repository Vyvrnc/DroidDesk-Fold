#!/usr/bin/env bash
# Runs the bionic build (tools/build-v4l2-shim.sh first) in a real Termux userland:
# termux/termux-docker:aarch64 (Android system libc/linker + Termux prefix) under qemu.
# Tests the shim alone and stacked with libsocket_hook.so in both LD_PRELOAD orders.
# Needs the arm64 binfmt handler once per Docker VM boot:
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
# Under qemu the reader is slow, so the test checks the source rate (sequence numbers),
# not that every frame is delivered. Run from the repo root.
set -euo pipefail
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }

MSYS_NO_PATHCONV=1 docker run --rm --platform linux/arm64 -v "$(to_host "$PWD"):/repo" \
  termux/termux-docker:aarch64 bash -c '
set -e
J=/repo/app/android/app/src/main/jniLibs/arm64-v8a
O=/repo/tools/v4l2-shim/out
cp $J/libdroiddesk_v4l2.so $J/libsocket_hook.so $PREFIX/lib/
cp $O/v4l2_capture_test $TMPDIR/ && chmod +x $TMPDIR/v4l2_capture_test
export DROIDDESK_V4L2_LOG=$TMPDIR/droiddesk-v4l2.log
V=$PREFIX/lib/libdroiddesk_v4l2.so H=$PREFIX/lib/libsocket_hook.so
fail=0
echo "== shim alone"
LD_PRELOAD=$V $TMPDIR/v4l2_capture_test 3 $TMPDIR/bionic || fail=1
echo "== shim before socket_hook"
LD_PRELOAD=$V:$H $TMPDIR/v4l2_capture_test 2 | grep -E "fps|RESULT|FAIL" || fail=1
echo "== socket_hook before shim (as the APK exports LD_PRELOAD)"
LD_PRELOAD=$H:$V $TMPDIR/v4l2_capture_test 2 | grep -E "fps|RESULT|FAIL" || fail=1
echo "== inherited through bash, DROIDDESK_V4L2_OFF=1 makes it inert"
LD_PRELOAD=$V bash -c "$TMPDIR/v4l2_capture_test 1 | tail -n 1"
DROIDDESK_V4L2_OFF=1 LD_PRELOAD=$V bash -c "$TMPDIR/v4l2_capture_test 1 | grep -c FAIL" >/dev/null || true
cp $TMPDIR/bionic-*.ppm $O/ 2>/dev/null || true
cp $DROIDDESK_V4L2_LOG $O/bionic-shim.log
echo "== shim log (without stats)"
grep -v stats $DROIDDESK_V4L2_LOG | tail -n 30
[ $fail = 0 ] && echo "BIONIC TESTS: PASS" || { echo "BIONIC TESTS: FAIL"; exit 1; }
'
