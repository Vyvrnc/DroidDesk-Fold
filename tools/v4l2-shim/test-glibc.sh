#!/usr/bin/env bash
# Builds libdroiddesk_v4l2.so for glibc (Debian trixie, x86_64) and tests it with the
# Firefox-sequence test program, v4l2-ctl and ffmpeg. The shim logic is the same as on
# bionic; this covers the parts that need real V4L2 clients. Run from the repo root.
# Output (logs, PPM frames) goes to tools/v4l2-shim/out/ (git-ignored).
set -euo pipefail
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }
mkdir -p tools/v4l2-shim/out

# v4l2-ctl identifies the node through /sys/dev/char/81:0/uevent (Firefox does not), so the
# container gets a tmpfs there with a fake uevent.
MSYS_NO_PATHCONV=1 docker run --rm --tmpfs /sys/dev/char -v "$(to_host "$PWD"):/repo" debian:trixie bash -c '
set -e
mkdir -p /sys/dev/char/81:0 && printf "MAJOR=81\nMINOR=0\nDEVNAME=video0\n" > /sys/dev/char/81:0/uevent
apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc libc6-dev v4l-utils ffmpeg >/dev/null 2>&1
S=/repo/tools/v4l2-shim O=/repo/tools/v4l2-shim/out
gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -U_FORTIFY_SOURCE \
  -o /tmp/libdroiddesk_v4l2.so $S/v4l2_shim.c $S/frame_source_pattern.c -ldl -lpthread
gcc -O2 -Wall -o /tmp/v4l2_capture_test $S/v4l2_capture_test.c
export DROIDDESK_V4L2_LOG=$O/glibc-shim.log
rm -f $DROIDDESK_V4L2_LOG
P=/tmp/libdroiddesk_v4l2.so
fail=0

echo "== Firefox call sequence"
LD_PRELOAD=$P /tmp/v4l2_capture_test 3 $O/glibc || fail=1

echo "== v4l2-ctl --all"
LD_PRELOAD=$P v4l2-ctl -d /dev/video0 --all || fail=1
echo "== v4l2-ctl --list-formats-ext"
LD_PRELOAD=$P v4l2-ctl -d /dev/video0 --list-formats-ext || fail=1
echo "== v4l2-ctl --stream-mmap (YUYV 640x480, 90 frames)"
LD_PRELOAD=$P v4l2-ctl -d /dev/video0 --set-fmt-video=width=640,height=480,pixelformat=YUYV \
  --stream-mmap=4 --stream-count=90 --stream-to=/tmp/v.raw 2>&1 | tail -n 3 || fail=1
sz=$(stat -c %s /tmp/v.raw); echo "stream-to: $sz bytes (expect $((640*480*2*90)))"
[ "$sz" = $((640*480*2*90)) ] || fail=1

echo "== ffmpeg -f v4l2 (I420 1280x720@30, 90 frames)"
LD_PRELOAD=$P ffmpeg -hide_banner -loglevel info -f v4l2 -input_format yuv420p -video_size 1280x720 \
  -framerate 30 -i /dev/video0 -frames:v 90 -f null - 2>&1 | grep -E "Stream #0|frame=|Input #0|error|Error" | tail -n 5
LD_PRELOAD=$P ffmpeg -hide_banner -loglevel error -f v4l2 -input_format nv12 -video_size 640x480 \
  -i /dev/video0 -frames:v 1 -update 1 -y $O/glibc-ffmpeg-nv12.png || fail=1

echo "== passthrough: shim loaded, ordinary tools unaffected"
LD_PRELOAD=$P bash -c "ls / >/dev/null && dd if=/dev/zero of=/tmp/z bs=1M count=8 status=none && md5sum /tmp/z >/dev/null" || fail=1

# PPM frames of this run and of test-bionic.sh -> PNG
for f in $O/*.ppm; do ffmpeg -loglevel error -y -i "$f" "${f%.ppm}.png" && rm -f "$f"; done

echo "== shim log (tail)"
tail -n 25 $DROIDDESK_V4L2_LOG
[ $fail = 0 ] && echo "GLIBC TESTS: PASS" || { echo "GLIBC TESTS: FAIL"; exit 1; }
'
