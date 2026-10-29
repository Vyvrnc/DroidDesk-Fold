#!/usr/bin/env bash
# Builds libdroiddesk_v4l2.so for glibc (Debian trixie, x86_64) and tests it with the
# Firefox-sequence test program, v4l2-ctl and ffmpeg. The shim logic is the same as on
# bionic; this covers the parts that need real V4L2 clients. The camera source runs against
# fake_camera_bridge (the app protocol without Android): frames, a second client (busy), the
# bridge going away mid-stream (reconnect) and an error answer (test pattern fallback).
# Run from the repo root.
# Output (logs, PPM frames) goes to tools/v4l2-shim/out/ (git-ignored).
set -euo pipefail
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }
mkdir -p tools/v4l2-shim/out

# v4l2-ctl identifies the node through /sys/dev/char/81:0/uevent (Firefox does not), so the
# container gets a tmpfs there with a fake uevent.
MSYS_NO_PATHCONV=1 docker run --rm --tmpfs /sys/dev/char -v "$(to_host "$PWD"):/repo" debian:trixie bash -c '
set -e
for m in 0 1; do mkdir -p /sys/dev/char/81:$m && printf "MAJOR=81\nMINOR=$m\nDEVNAME=video$m\n" > /sys/dev/char/81:$m/uevent; done
apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc libc6-dev v4l-utils ffmpeg socat >/dev/null 2>&1
S=/repo/tools/v4l2-shim O=/repo/tools/v4l2-shim/out
gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -U_FORTIFY_SOURCE \
  -o /tmp/libdroiddesk_v4l2.so $S/v4l2_shim.c $S/frame_source_bridge.c $S/frame_source_pattern.c -ldl -lpthread
gcc -O2 -Wall -o /tmp/v4l2_capture_test $S/v4l2_capture_test.c
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o /tmp/fake_camera_bridge $S/fake_camera_bridge.c \
  $S/frame_source_pattern.c -lpthread
export DROIDDESK_V4L2_LOG=$O/glibc-shim.log
rm -f $DROIDDESK_V4L2_LOG
P=/tmp/libdroiddesk_v4l2.so
fail=0

echo "== app side: camera_jni.c conversion (YUV_420_888 -> I420, rotation, crop, scale)"
gcc -O2 -Wall -Wextra -Wno-unused-parameter -I$S/camera_jni_test/stub -o /tmp/camera_jni_test \
  $S/camera_jni_test/test.c && /tmp/camera_jni_test || fail=1

echo "== Firefox call sequence (no app: the camera source falls back to the test pattern)"
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

echo "== camera source: frames from fake_camera_bridge (the app protocol, no Android)"
/tmp/fake_camera_bridge 2>$O/glibc-fake-bridge.log &
FAKE=$!
sleep 0.5
V4L2_TEST_SOURCE=bridge LD_PRELOAD=$P /tmp/v4l2_capture_test 3 $O/glibc-bridge > /tmp/bridge.out &
CAP=$!
sleep 1
echo "-- second client while streaming:"
busy=$(echo "open v=1 width=640 height=480 fps=30 format=I420 facing=front" | socat -t4 - ABSTRACT-CONNECT:droiddesk.camera)
echo "   $busy"
case "$busy" in "err busy"*) ;; *) fail=1 ;; esac
wait $CAP || fail=1
grep -E "fps|RESULT|FAIL" /tmp/bridge.out
n=$(grep -c "\[$CAP/.*camera bridge: ok v=1" $DROIDDESK_V4L2_LOG || true)
echo "   $n of 4 streams from the bridge"
[ "$n" = 4 ] || fail=1

echo "== /dev listing (Chromium enumerates by readdir): video0 and video1 exactly once"
n=$(LD_PRELOAD=$P ls /dev | grep -c -E "^video[01]$" || true)
echo "   $n of 2 names; plain ls: $(ls /dev | grep -c -E "^video[01]$" || true)"
[ "$n" = 2 ] || fail=1
n=$(LD_PRELOAD=$P ls -f /dev | sort | uniq -d | wc -l)
[ "$n" = 0 ] || { echo "   duplicate names in the listing"; fail=1; }

echo "== two devices: video0 = front, video1 = back (card names, facing sent to the bridge)"
for d in 0 1; do
  LD_PRELOAD=$P v4l2-ctl -d /dev/video$d --info 2>/dev/null | grep -E "Card type|Bus info" | sed "s/^/   video$d /"
done
LD_PRELOAD=$P v4l2-ctl -d /dev/video0 --info | grep -q "přední kamera" || fail=1
LD_PRELOAD=$P v4l2-ctl -d /dev/video1 --info | grep -q "zadní kamera" || fail=1
LD_PRELOAD=$P ffmpeg -hide_banner -loglevel error -f v4l2 -input_format yuv420p -video_size 640x480 \
  -i /dev/video1 -frames:v 5 -f null - || fail=1
tail -n 20 $DROIDDESK_V4L2_LOG | grep -q "open /dev/video1" || { echo "no open of video1"; fail=1; }
tail -n 20 $DROIDDESK_V4L2_LOG | grep -q "camera bridge: ok v=1.*facing=back" || { echo "video1 not the back camera"; fail=1; }
echo "-- switching cameras: video1 opens while video0 still streams, gets the camera when it is free"
LD_PRELOAD=$P timeout 20 ffmpeg -hide_banner -loglevel error -f v4l2 -input_format yuv420p -video_size 640x480 \
  -i /dev/video0 -frames:v 150 -f null - &
FF0=$!
sleep 1
LD_PRELOAD=$P timeout 20 ffmpeg -hide_banner -loglevel error -f v4l2 -input_format yuv420p -video_size 640x480 \
  -i /dev/video1 -frames:v 30 -f null - && echo "   video1 got its frames" || { echo "   video1 failed"; fail=1; }
wait $FF0 || fail=1
grep -q "camera busy (another stream), retrying" $DROIDDESK_V4L2_LOG || { echo "no busy retry in the log"; fail=1; }
tail -n 30 $DROIDDESK_V4L2_LOG | grep -q "facing=back" || { echo "video1 did not get the back camera"; fail=1; }

echo "== camera source: bridge lost mid-stream (app restart), reconnects, ffmpeg gets its 150 frames"
LD_PRELOAD=$P timeout 30 ffmpeg -hide_banner -loglevel error -f v4l2 -input_format yuv420p -video_size 640x480 \
  -framerate 30 -i /dev/video0 -frames:v 150 -f null - &
FF=$!
sleep 1.5; kill $FAKE; wait $FAKE 2>/dev/null || true
sleep 2.5
/tmp/fake_camera_bridge 2>>$O/glibc-fake-bridge.log &
FAKE=$!
wait $FF && echo "   ffmpeg done" || { echo "   ffmpeg failed"; fail=1; }
grep -q "camera bridge: reconnected" $DROIDDESK_V4L2_LOG || { echo "no reconnect in the log"; fail=1; }
kill $FAKE; wait $FAKE 2>/dev/null || true

echo "== camera source: app answers no-permission -> test pattern"
FAKE_CAMERA_ANSWER=no-permission /tmp/fake_camera_bridge fake-denied 2>>$O/glibc-fake-bridge.log &
FAKE=$!
sleep 0.5
DROIDDESK_CAMERA_SOCKET=fake-denied LD_PRELOAD=$P /tmp/v4l2_capture_test 1 | grep -E "RESULT|FAIL" || fail=1
grep -q "err no-permission test\" -> test pattern" $DROIDDESK_V4L2_LOG || { echo "no fallback line"; fail=1; }
kill $FAKE; wait $FAKE 2>/dev/null || true

echo "== passthrough: shim loaded, ordinary tools unaffected"
LD_PRELOAD=$P bash -c "ls / >/dev/null && dd if=/dev/zero of=/tmp/z bs=1M count=8 status=none && md5sum /tmp/z >/dev/null" || fail=1

# PPM frames of this run and of test-bionic.sh -> PNG
for f in $O/*.ppm; do ffmpeg -loglevel error -y -i "$f" "${f%.ppm}.png" && rm -f "$f"; done

echo "== shim log (tail)"
tail -n 25 $DROIDDESK_V4L2_LOG
[ $fail = 0 ] && echo "GLIBC TESTS: PASS" || { echo "GLIBC TESTS: FAIL"; exit 1; }
'
