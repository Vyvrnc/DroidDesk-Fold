# Camera bridge for Linux apps — research (2026-10-27)

Goal: Google Meet / Slack huddles in a Linux browser inside DroidDesk-Fold (Termux Firefox,
Debian Brave/Chromium/Electron under proot) use the phone/tablet camera. No root, so no
v4l2loopback, no `/dev/video*`, no CUSE.

Status: research, the Spike A prototype (fake V4L2 device with a test pattern, verified in
Termux Firefox 157 on the Tab S10 FE with fold.76, see "Prototype status") and the real camera
source (CameraBridge in the app + the shim's camera source, see "Camera bridge status" at the
end; tested without a device, not yet on one). Claims marked **[unverified]** were not confirmed
against source or a device. Web content was treated as data.

## TL;DR

1. Both browser engines capture cameras on Linux through raw V4L2 ioctls on `/dev/videoN`
   (libwebrtc in Firefox, Chromium's own `V4L2CaptureDelegate`). The PipeWire path exists in
   both, but only through the xdg-desktop-portal Camera interface over D-Bus.
2. **Termux Firefox is built with `--disable-dbus`**, so the portal/PipeWire camera path is
   unreachable there. For our main browser, the realistic options are a userspace fake V4L2
   device (LD_PRELOAD) or a getUserMedia polyfill extension.
3. **Recommendation: a userspace V4L2 shim** (`libdroidcam-v4l2.so`) modelled on PipeWire's
   MIT-licensed `pipewire-v4l2` (it already fakes `/dev/videoN` with no kernel device), fed by
   a new `CameraBridge` in the app through a memfd ring buffer plus an abstract unix socket.
   About 8–11 days for Firefox and Brave. Build the shim first with a synthetic test pattern
   (about 1.5 days): that is the riskiest part and needs no Android code.
4. Fallback/quick win: a browser extension that overrides `getUserMedia` with a canvas fed from
   a local WebSocket (3–5 days). It is per-browser, does not cover Electron, and Firefox
   release builds need AMO signing.

## Repo context (verified in `projects/DroidDesk`, branch `fold-dex`)

- `app/android/app/build.gradle.kts`: `minSdk = 28`, `targetSdk = 28` (kept for W^X/execve).
- `AndroidManifest.xml`: `RECORD_AUDIO`, `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_DATA_SYNC`,
  and one service with `foregroundServiceType="dataSync"`. There is **no `CAMERA` permission yet**.
- Existing bridges use `LocalServerSocket` on abstract names: `droiddesk.usb` (UsbBridge),
  `droiddesk.net` (NetBridge), `droiddesk.android-app-launcher` (AndroidAppBridge). A camera
  bridge would follow the same pattern: `droiddesk.camera`.
- `app/assets/socket_hook.c` (816 lines) is LD_PRELOADed into the Termux userspace. It hooks
  open/open64/openat/openat64/fopen, the stat family, access, mkdir, …, connect/bind (path
  rewrite), dlopen and opendir. It does **not** hook ioctl/mmap/close/dup.
- The microphone is requested in `MainActivity.requestMicrophoneOnce()` ("PulseAudio source").
  The camera permission prompt should follow the same once-only pattern.
- Native code already exists (`app/src/main/cpp/blk_shim.c`), so JNI for memfd/SCM_RIGHTS fits in.
- Termux Firefox web apps (Slack profile) are documented in `docs/backlog.md` → "Web apps".
  Meet would be the next web app.

## 1. How Linux browsers capture video

### 1.1 Termux Firefox (bionic) — what is actually built

Termux recipe (`x11-packages/firefox`, version 158.0.1):
https://github.com/termux/termux-packages/blob/master/x11-packages/firefox/mozconfig.cfg

- `--disable-dbus`, `--disable-sandbox`, `--enable-audio-backends=pulseaudio`,
  `--enable-default-toolkit=cairo-gtk3-x11-only`, `MOZILLA_OFFICIAL=1` + official branding.
- `build.sh` adds `CXXFLAGS+=" -U__ANDROID__"`, so libwebrtc is compiled as Linux and the
  Linux V4L2 capture module should be present. WebRTC itself is not disabled.
  **[unverified on device]**: confirm with `strings $PREFIX/lib/firefox/libxul.so | grep /dev/video`.
- Consequences:
  - **No D-Bus means no xdg-desktop-portal, so no PipeWire camera.** The pref
    `media.webrtc.camera.allow-pipewire` cannot help. Set it to `false` in the profile anyway,
    so Firefox cannot take a portal path and get stuck there (Mozilla bug 1849608 describes a
    portal denial with no fallback: https://bugzilla.mozilla.org/show_bug.cgi?id=1849608).
  - No content sandbox, so the camera capture code runs in a process that inherits LD_PRELOAD
    and can open whatever we fake.
  - Release-branded build: unsigned extensions cannot be installed permanently. This matters
    for option (c).

### 1.2 Firefox and PipeWire in general

- Firefox can capture through PipeWire via the xdg-desktop-portal Camera interface, behind the
  pref `media.webrtc.camera.allow-pipewire`. Fedora 41+ ships it enabled. Upstream default per
  version: **[unverified]**. Sources: https://jgrulich.cz/2024/08/19/making-pipewire-default-option-for-firefox-camera-handling ,
  https://www.phoronix.com/news/Fedora-41-Firefox-PipeWire-Cam
- libwebrtc always gets the PipeWire fd from the portal (`OpenPipeWireRemote`). The 2023 split
  only lets Firefox's frontend make the portal request itself; there is no direct connection to
  the PipeWire socket: https://webrtc.googlesource.com/src/+/56d126074e5fb62a65e4e14cce44466ce7297770%5E%21/
- Debian trixie's `firefox-esr` under proot could use the portal if D-Bus, PipeWire and
  xdg-desktop-portal ran in proot. See option (b).

### 1.3 libwebrtc V4L2 path (Firefox)

Source: https://webrtc.googlesource.com/src/+/refs/heads/main/modules/video_capture/linux/device_info_v4l2.cc
and `.../video_capture_v4l2.cc`. Firefox ships a fork, so details can drift **[verify against
the Firefox 158 tree]**.

- Enumeration: `open("/dev/video%d", O_RDONLY)` for n = 0..63, then `VIDIOC_QUERYCAP`, which
  must have `V4L2_CAP_VIDEO_CAPTURE` in `device_caps`. `card` becomes the name and `bus_info` the
  unique ID. Capabilities come from **`VIDIOC_TRY_FMT`** over a hard-coded format list × 13 sizes
  (128x96 … 1920x1080). The FPS is guessed (30, or 15 for ≥800 px wide non-MJPEG). No
  ENUM_FRAMESIZES. No inotify in upstream (Mozilla's fork may add hotplug watching **[unverified]**).
- Capture: `open(O_RDWR|O_NONBLOCK)`, then `ENUM_FMT`, `S_FMT`, `G_PARM`/`S_PARM`
  (`V4L2_CAP_TIMEPERFRAME`), `REQBUFS` (`V4L2_MEMORY_MMAP`), `QUERYBUF`, `mmap(MAP_SHARED, fd,
  m.offset)`, `QBUF`, `STREAMON`, a loop of `select()` (1 s timeout) + `DQBUF`/`QBUF`, then
  `STREAMOFF`. No `read()`.
- Format preference. SD: YUV420 (I420) > YVU420 > YUYV > UYVY > NV12 > RGB… > MJPEG. HD
  (>640x480): MJPEG first, then YUV420 … **For the shim, offer only I420 (+ optionally NV12).**
  Then there is no JPEG decode in the browser and HD does not drop to MJPEG.

### 1.4 Chromium / Brave / Electron V4L2 path

Sources: https://chromium.googlesource.com/chromium/src/+/refs/heads/main/media/capture/video/linux/video_capture_device_factory_v4l2.cc ,
https://chromium.googlesource.com/chromium/src/+/refs/heads/main/media/capture/video/linux/v4l2_capture_delegate.cc

- Enumeration: **directory listing** of `/dev/` with pattern `video*` (`base::FileEnumerator`,
  so opendir/readdir), not a fixed probe loop. It reads sysfs
  `/sys/class/video4linux/videoN/device/{interface,../idVendor,../idProduct}` for name/model and
  falls back to `cap.card`. Ioctls: `QUERYCAP` (capture, not output), `ENUM_FMT`,
  `ENUM_FRAMESIZES` (**discrete only**), `ENUM_FRAMEINTERVALS` (discrete only), and `QUERYCTRL`
  (pan/tilt/zoom).
- Capture: `QUERYCAP`, `ENUM_FMT`, `S_FMT`, `G_PARM`/`S_PARM`, `REQBUFS` (MMAP only),
  `QUERYBUF`, `mmap`, `QBUF`, `STREAMON`, **`poll(POLLIN|POLLPRI, 1000 ms)`**, `DQBUF`,
  `STREAMOFF`. Plus `SUBSCRIBE_EVENT`/`DQEVENT`, `QUERYCTRL`/`G_CTRL`/`S_CTRL`/`S_EXT_CTRLS`
  (power-line frequency and photo controls). These may fail with `EINVAL`/`ENOTTY`, but each
  must be checked on a real run **[unverified which failures are fatal]**.
- Preference: I420 > NV12 > Y16 … > YUYV > RGB24 > MJPEG. MJPEG moves to the front above
  640x480, unless the device doesn't offer it. Same conclusion: offer I420/NV12 only.
- PipeWire camera: chrome://flags "PipeWire camera support" (`#enable-webrtc-pipewire-camera`),
  usable since about M128 (M127 broken by CFI). It also goes through the portal. There are user
  reports of a black Meet window with the flag on:
  https://discussion.fedoraproject.org/t/intel-ipu6-ovti02e1-camera-not-working-in-chrome-and-other-applications/196023
- Test-only alternative: `--use-fake-device-for-media-stream --use-file-for-fake-video-capture=x.y4m`
  plays a file as the camera. Whether it can read a live FIFO: **[unverified, likely not]**.
  Useful only as a pipeline smoke test.

### 1.5 Minimal V4L2 surface our fake device must implement

| Call | Firefox | Chromium | Notes |
|---|---|---|---|
| open `/dev/videoN` | probe 0..63 | from `/dev` listing | Chromium also needs the name to show up in readdir |
| QUERYCAP | ✓ | ✓ | `device_caps = VIDEO_CAPTURE \| STREAMING`, set `V4L2_CAP_DEVICE_CAPS` |
| ENUM_FMT / TRY_FMT / S_FMT / G_FMT | TRY+ENUM+S | ENUM+S | I420, NV12 |
| ENUM_FRAMESIZES / FRAMEINTERVALS | – | ✓ (discrete) | 640x480, 1280x720, (1920x1080); 30/15 fps |
| G_PARM / S_PARM | ✓ | ✓ | `CAP_TIMEPERFRAME` |
| REQBUFS / QUERYBUF / QBUF / DQBUF | ✓ | ✓ | MMAP, 2–4 buffers |
| mmap / munmap on device fd | ✓ | ✓ | hook required |
| select / poll readiness | select | poll | free if the fd is a real eventfd/socket |
| STREAMON / STREAMOFF | ✓ | ✓ | start/stop Android camera |
| QUERYCTRL/G_CTRL/S_CTRL, SUBSCRIBE_EVENT | – | ✓ | return EINVAL (no controls) |
| close / dup | ✓ | ✓ | refcount fake fds |

## 2. Options without root

### (a) Userspace V4L2 emulation via LD_PRELOAD — recommended

**Prior art (verified):**
- **PipeWire `pipewire-v4l2` / `pw-v4l2`** (MIT). An LD_PRELOAD library that exports `open`,
  `__open_2`, `open64`, `openat`, `openat64` (+ `__*_2` fortify variants), `dup`, `close`,
  `mmap`, `mmap64`, `munmap`, and `ioctl(int, unsigned long, ...)`. Any path starting with
  `/dev/videoN` is captured without needing the node to exist (`realpath` + prefix match). It
  returns an **eventfd** as the device fd, so `poll`/`select` work unhooked and the eventfd is
  written when a buffer is ready.
  Source: https://github.com/PipeWire/pipewire/tree/master/pipewire-v4l2/src (v4l2-func.c,
  pipewire-v4l2.c); man page https://man.archlinux.org/man/pw-v4l2.1.en.
  **This is the template to copy (licence permits).**
- libcamera `v4l2-compat` / `libcamerify`: the same technique, LGPL. It maps real
  `/dev/video` nodes to libcamera cameras. https://lists.libcamera.org/pipermail/libcamera-devel/2022-January/028673.html
- `dv4lstart` (Debian `dv4l`): an old LD_PRELOAD fake V4L device for dv1394 camcorders, which
  shows the technique is old and works. https://manpages.debian.org/wheezy/dv4l/dv4lstart.1.en.html
- `libv4l2` plugins: only for apps that link libv4l2. Firefox and Chromium issue raw ioctls,
  so **not usable**.
- `v4l2loopback`, `akvcam`, `vcam`, `v4l2-relayd` (relayd is a daemon on top of a loopback
  device): all are kernel modules or need one, so **not usable**.

**Design for DroidDesk:**
- `libdroidcam-v4l2.so`: standalone C, about 800–1200 lines, no PipeWire dependency, built
  twice:
  - **bionic** (Termux clang/NDK): bionic's prototype is `int ioctl(int, int, ...)`. Define our
    export to match the header, or `dlsym` conflicts. Load it via LD_PRELOAD next to
    `socket_hook.so`, only for browser launchers (wrapper `droiddesk-cam firefox …`) to keep
    ioctl/mmap hooks out of every process. Alternatively merge it into `socket_hook.c` later.
    socket_hook already rewrites open paths, so the camera check must run **before** the
    rewrite, or `socket_hook` must pass `/dev/video*` through unchanged.
  - **glibc** (built inside Debian with gcc): `ioctl(int, unsigned long, ...)`, plus
    `__open_2`/`__open64_2`/`__openat_2` and `mmap64`. Set it through `LD_PRELOAD` in the proot
    login environment or per app (Brave `.desktop` Exec wrapper). LD_PRELOAD works under proot
    because the guest ld.so loads it. Chromium's zygote and utility processes inherit it.
- Fake fd = connected `SOCK_SEQPACKET` socket to `@droiddesk.camera` (or an eventfd plus a
  control socket). The app sends a 1-byte notify per frame, so `select`/`poll` readiness is
  native. `DQBUF` drains the notify, copies the newest frame from the shared ring into the
  client's buffer, and returns `EAGAIN` when there is nothing new (fd is `O_NONBLOCK`).
- Client buffers: in `mmap(fd=fake)`, return an anonymous `MAP_SHARED` region per `QUERYBUF`
  offset. One memcpy per frame (1280x720 I420 = 1.38 MB; at 30 fps about 41 MB/s, negligible).
  This avoids cross-process buffer-ownership protocols. Zero-copy is possible later.
- Chromium device discovery: readdir of `/dev`. Under proot, bind a placeholder regular file
  (`proot-distro login debian --bind $PREFIX/var/run/droidcam/video0:/dev/video0`). The
  alternative is hooking `readdir` for `/dev`, which is more fragile. The sysfs reads just fail
  and the name comes from `cap.card` ("DroidDesk Back Camera").
- Two fake devices: `/dev/video0` = back, `/dev/video1` = front. Browsers' own device pickers
  then do the front/back switch. Fold 7 has more cameras (cover/under-display). Expose only
  primary back/front by `LENS_FACING`.
- Risks: missing a code path (fortify variants, `ioctl` via a different symbol, `syscall()` use),
  thread safety of the fd table, `fork` after open, Firefox fork differences, Chromium
  treating a failed control ioctl as fatal. All of these show up immediately in the spike.

### (b) PipeWire camera node + xdg-desktop-portal Camera

- Termux: `pipewire` 1.4.9 is packaged in the main repo, **built with `-Ddbus=disabled`**,
  wireplumber as session manager, no gstreamer
  (https://github.com/termux/termux-packages/blob/master/packages/pipewire/build.sh).
  `xdg-desktop-portal` exists in x11-packages. Termux Firefox has no D-Bus, so **the portal path
  is dead for Termux Firefox** regardless.
- Debian proot: would need a dbus session bus, pipewire + wireplumber, xdg-desktop-portal +
  backend (gtk) for the permission dialog, plus a custom PipeWire source node fed by the app
  (Camera role, so the portal reports a camera present). Then Chromium/Brave with the PipeWire
  camera flag, or firefox-esr with the pref. That is a lot of moving daemons inside proot. The
  portal's Camera access check and permission store add failure modes (see bug 1849608).
- Variant (b′): run PipeWire only as a transport and use stock **`pw-v4l2`** as the shim, with
  no portal. Whether Termux's build ships `libpw-v4l2.so` is **[unverified]**: check
  `pkg files pipewire | grep v4l2`. Debian has `pipewire-v4l2`. This still needs a
  PipeWire-producing client fed by the app, a pipewire+wireplumber pair next to the existing
  PulseAudio, and the Chromium readdir problem. It costs more than our own shim and gains little.
- Effort: (b) 6–10 days, high risk, Debian only. (b′) 5–8 days, medium risk. **Not recommended.**

### (c) getUserMedia polyfill extension + local stream from the app

- The app serves frames on `127.0.0.1:<port>`: WebSocket with raw I420 (preferred) or MJPEG
  (`multipart/x-mixed-replace`). Termux/proot processes share the app's network namespace, so
  localhost is reachable.
- A content script in the page's MAIN world at `document_start` overrides
  `navigator.mediaDevices.getUserMedia` and `enumerateDevices`. For `video`, it draws frames into
  a `<canvas>` (`VideoFrame` from an I420 buffer via WebCodecs, or `createImageBitmap` from JPEG),
  calls `canvas.captureStream(30)`, and merges the real audio track from the original gUM call.
  It also adds a fake `videoinput` entry so Meet's device picker shows "DroidDesk camera".
  Firefox supports `world: "MAIN"` from about v128 **[verify]**. Otherwise use
  `wrappedJSObject`/`exportFunction` from an isolated content script.
- Prior art: the AMO add-on "Image Camera" replaces the getUserMedia camera with an image or
  video URL: https://addons.mozilla.org/en-US/firefox/addon/image-camera/ (not audited).
- Pitfalls:
  - The canvas must stay origin-clean. Serve with `Access-Control-Allow-Origin: *` +
    `crossOrigin="anonymous"`, or use WebSocket (no CORS).
  - Termux Firefox is an official release build, so a **permanent install needs AMO signing**
    (unlisted self-distribution, automated through `web-ext sign`). Otherwise it is temporary
    via about:debugging and lost at restart.
  - Chrome's Local Network Access permission for public-site → localhost requests (Chrome
    ~141/142) may prompt or block **[unverified for WebSocket and for Brave]**.
  - Meet may break the polyfill in future UI changes. The extra canvas + captureStream step
    costs CPU. **Electron apps are not covered.**
- Effort: 3–5 days (server in app 1, extension 2, signing/packaging 0.5–1, testing 1). Low
  technical risk, medium maintenance risk.

### (d) Meet in an Android WebView/Chrome instead

Works today with the native camera, but it leaves the Linux desktop (separate window,
different session, no XFCE integration). Out of scope; mention it to users as the emergency
fallback.

## 3. Android side (`CameraBridge`)

- **Permission:** add `<uses-permission android:name="android.permission.CAMERA"/>` and
  `<uses-feature android:name="android.hardware.camera.any" android:required="false"/>`.
  Request it once like the microphone (`MainActivity`), or lazily on first `open` of
  `/dev/videoN` (bridge posts a notification/activity to grant it).
- **Foreground/background rules:**
  - Android 9+: background apps cannot access the camera at all
    (https://developer.android.com/about/versions/pie/android-9.0-changes-all#bg-sensor-access).
    While `DesktopActivity` is visible we are foreground, which is the normal case for a call.
  - Android 11+: an FGS started from the background cannot use the camera ("while-in-use").
    The type requirement (`camera`) applies to apps targeting 30+, and `FOREGROUND_SERVICE_CAMERA`
    to targeting 34+ (https://developer.android.com/about/versions/11/privacy/foreground-services ,
    https://developer.android.com/about/versions/14/changes/fgs-types-required).
  - With targetSdk 28 we skip the type declarations. Whether a pre-R app's existing `dataSync`
    FGS implicitly keeps camera capability when the user switches to another Android app is
    **[unverified, test on Android 16]**. Expected behaviour: the camera keeps running while
    DroidDesk is visible (including DeX/multi-window), and may stop when DroidDesk is fully
    backgrounded. The shim should then deliver a "camera paused" frame instead of an error.
  - Multi-window/DeX: CameraService can evict us for a higher-priority (focused top) app using
    the camera. Handle `onDisconnected` → reopen on focus.
  - The privacy toggle (quick settings camera off) produces an error. Show a placeholder frame
    and a notification.
- **Capture:** Camera2 (CameraX adds a lifecycle we don't need in a service; Camera2 is fine).
  Use `ImageReader(w, h, YUV_420_888, 3)` with a repeating `TEMPLATE_RECORD` request at a
  `CONTROL_AE_TARGET_FPS_RANGE` of 30 (or 15 for low power). Sizes 1280x720 (default) and
  640x480; 1920x1080 optional. YUV_420_888 is usually NV21-like (pixelStride 2). Convert to
  I420/NV12 in native code (libyuv `Android420ToI420`, or a hand loop) straight into the shared
  ring, which costs about 1–2 ms per 720p frame on these SoCs **[estimate]**.
- **Rotation/mirroring:** apply `SENSOR_ORIENTATION` + current display rotation (Fold
  folded/unfolded, Tab landscape) via libyuv `I420Rotate`. Don't mirror: browsers mirror the
  self-view themselves, and remote sides must see the unmirrored image.
- **Transport to Linux (recommended):** the native code creates a memfd (`memfd_create` syscall,
  or `ASharedMemory_create`, NDK API 26), holding a header (seqlock, width, height, format,
  frame counter, timestamp) + 3 slots. The server `@droiddesk.camera` (`LocalServerSocket`, like
  the other bridges) handles:
  - control messages: OPEN `{facing, w, h, fps, fmt}`, START, STOP;
  - sending the memfd via `LocalSocket.setFileDescriptorsForSend` (SCM_RIGHTS);
  - one notify byte per frame.
  The shim mmaps the memfd read-only. Latency added is under 1 ms, CPU is about one memcpy.
- **MJPEG over TCP** (only for option c or debugging): `YuvImage.compressToJpeg` costs CPU on
  both ends and adds 10–30 ms per frame **[estimate]**. Not for the V4L2 path.
- **Latency budget:** camera pipeline 60–120 ms (device dependent) + conversion < 5 ms + browser
  encode/network. The target glass-to-glass in Meet is ≤ 300 ms, comparable to a USB webcam.
- **ScreenMate One (Android Automotive 14):** may have no Camera2 camera, or only EVS cameras.
  Check `pm list features | grep camera` and `CameraManager.cameraIdList`. If it has none, the
  shim reports no devices.

## 4. Recommendation, effort, risks, plan

| Option | Termux Firefox | Debian Brave/Chromium | Electron | Effort (days) | Risk |
|---|---|---|---|---|---|
| (a) own V4L2 shim + CameraBridge | ✓ | ✓ (placeholder `/dev/video0` bind) | ✓ | **8–11** | medium (ioctl coverage) |
| (b) PipeWire + portal | ✗ (no D-Bus) | ✓ with flag | ✓? | 6–10 | high |
| (b′) PipeWire + stock pw-v4l2 | ? (pw-v4l2 in Termux unverified) | partial (readdir) | partial | 5–8 | medium-high |
| (c) gUM polyfill extension | ✓ (needs AMO signing) | ✓ (LNA risk) | ✗ | 3–5 | low tech / medium upkeep |
| (d) Android browser | n/a | n/a | n/a | 0 | out of scope |

Breakdown for (a): CameraBridge Kotlin+JNI (permission, Camera2, conversion, rotation, memfd,
socket) 2.5–3. Bionic shim + Firefox bring-up 3–4. glibc build + proot bind + Chromium/Brave
quirks 1.5–2. Device testing (Fold 7, Tab S10 FE, ScreenMate) + Meet/Slack 1–2.

**What to prototype first:**
0. Zero-code checks (about 1 h):
   - Termux Firefox with `media.navigator.streams.fake=true` on Meet's preview and
     https://webrtc.github.io/samples/src/content/getusermedia/gum/ . This proves the WebRTC
     encode/send path works in this build.
   - `strings libxul.so | grep -c /dev/video`, which shows whether V4L2 capture is compiled in.
   - Brave in proot with `--use-fake-device-for-media-stream`.
1. **Spike A (about 1.5 days):** bionic `libdroidcam-v4l2.so` that generates a moving
   test pattern itself (no Android side). Run `LD_PRELOAD=… firefox` and get the gUM sample page
   and the Meet preview to show bars. Log every ioctl seen. This settles the main unknown.
2. Spike B (about 0.5 day): same shim built for glibc, run in Brave under proot with the
   placeholder bind.
3. Then the real CameraBridge feeding the memfd ring, front/back, rotation.
4. Option (c) only if Spike A fails on something structural (for example, Firefox's fork
   bypassing libc).

**Success criteria (measure on the Fold 7 and the Tab S10 FE):**
- Meet "check audio and video" preview and a 2-party call show the DroidDesk camera in
  Firefox (Termux) and Brave (Debian). Slack huddle video works in the Slack Firefox profile.
- about:webrtc / brave://webrtc-internals: sending ≥ 24 fps at 1280x720 (or 640x480 at 30),
  with no "limited by CPU" for ≥ 10 min.
- Glass-to-glass latency ≤ 300 ms: film a millisecond stopwatch on a second device and compare
  it with the remote view.
- CPU overhead of the capture path (app + shim, excluding the browser's encoder) ≤ 15 % of one
  core at 720p30. Measure with `top -H` deltas, camera on vs `streams.fake`.
- 10× start/stop and front↔back switch without leaks (fd count, memfd count), with recovery
  after DroidDesk is backgrounded and foregrounded again. A 30-min call does not trigger
  thermal throttling worse than Android's own camera app.

## Sources

- Termux Firefox recipe: https://github.com/termux/termux-packages/tree/master/x11-packages/firefox
- Termux PipeWire recipe: https://github.com/termux/termux-packages/blob/master/packages/pipewire/build.sh
- libwebrtc V4L2: https://webrtc.googlesource.com/src/+/refs/heads/main/modules/video_capture/linux/
- libwebrtc portal split: https://webrtc.googlesource.com/src/+/56d126074e5fb62a65e4e14cce44466ce7297770%5E%21/
- Chromium V4L2: https://chromium.googlesource.com/chromium/src/+/refs/heads/main/media/capture/video/linux/
- Firefox PipeWire camera: https://jgrulich.cz/2024/08/19/making-pipewire-default-option-for-firefox-camera-handling ,
  https://bugzilla.mozilla.org/show_bug.cgi?id=1849608
- pipewire-v4l2: https://github.com/PipeWire/pipewire/tree/master/pipewire-v4l2 , https://man.archlinux.org/man/pw-v4l2.1.en
- libcamera wrapper: https://lists.libcamera.org/pipermail/libcamera-devel/2022-January/028673.html
- dv4l: https://manpages.debian.org/wheezy/dv4l/dv4lstart.1.en.html
- Image Camera add-on: https://addons.mozilla.org/en-US/firefox/addon/image-camera/
- Android: https://developer.android.com/about/versions/pie/android-9.0-changes-all ,
  https://developer.android.com/about/versions/11/privacy/foreground-services ,
  https://developer.android.com/about/versions/14/changes/fgs-types-required
- Chromium fake capture: https://codereview.chromium.org/2358093002 , https://testrtc.com/?p=176

## Prototype status (Spike A, 2026-10-27)

Built: `libdroiddesk_v4l2.so`, a user-space fake `/dev/video0` with a built-in moving test
pattern. No Android camera code yet. Opt-in only: nothing preloads it unless Firefox is
started through `tools/v4l2-shim/firefox-cam`.

### Files

| File | What |
|---|---|
| `tools/v4l2-shim/v4l2_shim.c` | The shim: open/openat (+64, fortify `__open_2`), close, dup/dup2/dup3, fcntl (F_DUPFD*), ioctl, mmap/mmap64, stat/lstat/fstat/fstatat (+64), access/faccessat. Bionic + glibc, LP64. |
| `tools/v4l2-shim/frame_source.h` | Frame source interface (`start` / `next_frame` / `stop`). The Camera2 bridge plugs in here. |
| `tools/v4l2-shim/frame_source_pattern.c` | Test pattern: scrolling 75 % bars, bouncing box, frame counter, `WxH fps`, a one-cell-per-frame ticker (one sweep per second = right fps), machine-readable counter in rows 0-1. |
| `tools/v4l2-shim/v4l2_capture_test.c` | Replays Firefox's V4L2 sequence (below) and checks frames, counter, rate, fd behaviour. |
| `tools/build-v4l2-shim.sh` | NDK r28c / API 29 build (Docker, same image as `build-socket-hook.sh`) → `app/android/app/src/main/jniLibs/arm64-v8a/libdroiddesk_v4l2.so` + bionic test binary in `tools/v4l2-shim/out/`. |
| `tools/v4l2-shim/test-bionic.sh` | Runs the bionic build in `termux/termux-docker:aarch64` (Android libc/linker under qemu), alone and stacked with `libsocket_hook.so` in both orders. |
| `tools/v4l2-shim/test-glibc.sh` | glibc build in Debian trixie; the test program plus `v4l2-ctl` and `ffmpeg -f v4l2`. |
| `tools/v4l2-shim/firefox-cam` | Launcher: own profile, `webgl.disabled`, `allow-pipewire=false`, `LD_PRELOAD`, `MOZ_LOG`. (Now `app/android/app/src/main/assets/droiddesk/firefox-cam`, installed as `$PREFIX/bin/firefox-cam`.) |
| `LinuxRuntime.writeDeviceTools()` | Copies the `.so` to `$PREFIX/lib` like `libdroiddesk_blk.so` (copy only, nothing loads it). |

`socket_hook.c` is unchanged. The two libraries chain through `dlsym(RTLD_NEXT)`, so the
order in `LD_PRELOAD` does not matter (tested both).

### How Termux Firefox finds and drives a camera (verified at `FIREFOX_157_0_RELEASE`)

- **Linux code path, not Android.** Termux patch
  [`0001-configure-treat-termux-as-linux.patch`](https://github.com/termux/termux-packages/blob/master/x11-packages/firefox/0001-configure-treat-termux-as-linux.patch)
  maps `*-android` to OS "GNU" / kernel "Linux", so `OS_TARGET` is Linux and
  [`dom/media/systemservices/moz.build`](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_157_0_RELEASE/dom/media/systemservices/moz.build#L32-L36)
  does not build the Android capture backend. The factory always allows V4L2 and only tries
  PipeWire with `MOZ_ENABLE_DBUS`, which `--disable-dbus` removes
  ([video_capture_factory.cc L18-L31](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_157_0_RELEASE/dom/media/systemservices/video_engine/video_capture_factory.cc#L18-L31)).
- **Process: the parent (main) Firefox process.** `CamerasChild` talks to `CamerasParent` over
  PBackground ([CamerasChild.cpp L42](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_157_0_RELEASE/dom/media/systemservices/CamerasChild.cpp#L42)),
  whose parent side lives in the main process. The build has `--disable-sandbox`, so nothing
  blocks the preload or the fake fd. Consequence: the shim must be in the env of the *first*
  Firefox process, so `firefox-cam` uses `--no-remote` and its own profile.
- **Enumeration: a fixed probe, no readdir, no sysfs.** `open("/dev/video%d", O_RDONLY)` for
  n = 0..63, `VIDIOC_QUERYCAP`, capture bit from `device_caps` when `V4L2_CAP_DEVICE_CAPS` is
  set; name = `card`, unique id = `bus_info`
  ([device_info_v4l2.cc L204-L299](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_157_0_RELEASE/third_party/libwebrtc/modules/video_capture/linux/device_info_v4l2.cc#L204-L299),
  [L388-L395](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_157_0_RELEASE/third_party/libwebrtc/modules/video_capture/linux/device_info_v4l2.cc#L388-L395)).
  Mozilla's fork adds an inotify thread on `/dev/` and `/dev/v4l/by-path/` for hotplug
  (L154-L176). On Android these watches fail quietly; the fake device is always "present".
- **Capabilities: `VIDIOC_TRY_FMT` over 15 formats × 13 sizes**, recorded when the returned
  size equals the asked size. The *asked* fourcc is recorded, so the shim fails TRY_FMT with
  EINVAL for formats it does not have (a spec-style substitution would list fake MJPEG/RGB
  modes). The fps is guessed, not asked: **15 for width ≥ 800 (non-MJPEG), else 30**
  (L397-L483). So pages get 1280x720 at 15 fps and 640x480 at 30 fps.
- **Open for capture: device matched by `bus_info` prefix** (`Init`,
  [video_capture_v4l2.cc L74-L112](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_157_0_RELEASE/third_party/libwebrtc/modules/video_capture/linux/video_capture_v4l2.cc#L74-L112)),
  so `bus_info` must be non-empty and unique per fake device.
- **Capture sequence** ([video_capture_v4l2.cc L127-L484](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_157_0_RELEASE/third_party/libwebrtc/modules/video_capture/linux/video_capture_v4l2.cc#L127-L484)):
  `open(O_RDWR|O_NONBLOCK)` → `ENUM_FMT` (preference: HD = MJPEG, I420, YV12, YUYV, UYVY,
  NV12 …; SD = I420 first) → `S_FMT` → `G_PARM` (+ `S_PARM` 1/maxFPS if `CAP_TIMEPERFRAME`) →
  `REQBUFS` 4 × MMAP → `QUERYBUF` + `mmap(MAP_SHARED, m.offset)` + `QBUF` → `STREAMON` →
  capture thread: **`poll(POLLIN, 1000 ms)`** + `DQBUF` + `QBUF` → stop: `munmap`,
  `STREAMOFF`, `close`. No `select`, no `read`, no controls, no events, no ENUM_FRAMESIZES.
  (§1.3 above says `select()`; Firefox 157 uses `poll()`.)

### What the shim implements

- `/dev/video0` (exact path) → a real **eventfd in semaphore mode**; its counter = filled
  buffers waiting for DQBUF, so `poll`/`select`/`epoll` need no hook. `O_NONBLOCK`/`O_CLOEXEC`
  from `open` carry over. An internal `F_DUPFD_CLOEXEC` copy keeps the eventfd alive for the
  producer; dups via `dup`/`dup2`/`dup3`/`fcntl` are tracked (refcount); the last close stops
  the stream and frees the buffers.
- Buffers: one memfd (`memfd_create` syscall, temp-file fallback), a page-aligned slot per
  buffer; `mmap` on the fake fd maps the memfd slot. A producer thread per stream calls the
  frame source at its own pace and drops frames when no buffer is queued (sequence counts on).
- ioctls: QUERYCAP (`driver droiddesk`, `card "DroidDesk Camera"`,
  `bus_info platform:droiddesk-camera-0`, CAPTURE|STREAMING + DEVICE_CAPS), ENUM_FMT / G_FMT /
  S_FMT / TRY_FMT (YU12, NV12, YUYV × 1280x720, 640x480), ENUM_FRAMESIZES /
  ENUM_FRAMEINTERVALS (discrete, 30 and 15 fps), G_PARM / S_PARM (1-30 fps), ENUMINPUT /
  G_INPUT / S_INPUT, REQBUFS (2-8, MMAP) / QUERYBUF / QBUF / DQBUF / STREAMON / STREAMOFF.
  Controls and standards: EINVAL / ENODATA; anything else ENOTTY (logged). `stat`/`access`
  see a char device 81:0.
- Fork: `pthread_atfork` keeps the lock consistent; a child does not inherit the producer.
- Env: `DROIDDESK_V4L2_LOG` (log path, default `$TMPDIR/droiddesk-v4l2.log`),
  `DROIDDESK_V4L2_DEBUG=1` (every ioctl/mmap), `DROIDDESK_V4L2_OFF=1` (inert).

### Test results (2026-10-27, no device)

- **Bionic** (`test-bionic.sh`, termux-docker aarch64 under qemu, Android `libc.so` and
  linker): PASS alone and stacked with `libsocket_hook.so` in both orders. Firefox sequence:
  exactly one device, 6 capabilities (YU12/NV12/YUYV × 640x480@30, 1280x720@15). Capture
  YU12 1280x720@15 → 15.0 fps, YU12 640x480 → 30.0, YUYV 640x480 → 30.0, NV12 1280x720 →
  30.0 fps; 0 poll timeouts, 0 dropped, image counter = `v4l2_buffer.sequence` on every
  frame; dup / F_DUPFD / fstat / F_GETFL work; no POLLIN after STREAMOFF; `/dev/null` ioctls
  pass through (ENOTTY); inheritance through `bash` and `DROIDDESK_V4L2_OFF=1` checked.
- **glibc** (`test-glibc.sh`, Debian trixie x86_64): the same program PASS; `v4l2-ctl --all`,
  `--list-formats-ext`, `--stream-mmap=4 --stream-count=90` (YUYV 640x480, 30.00 fps, exact
  byte count); `ffmpeg -f v4l2` I420 1280x720 → 90 frames at 30 fps, NV12 640x480 → PNG.
  `v4l2-ctl` identifies nodes through `/sys/dev/char/81:0/uevent`; the test fakes that file
  in a tmpfs (Firefox never reads sysfs).
- Frames dumped as PNG in `tools/v4l2-shim/out/` (git-ignored) checked visually for all
  three formats.
- **Not tested: real Firefox.** No Termux Firefox could run here; that is the on-device step.

### On-device test (Fold 7 / Tab S10 FE)

1. Get the library into the prefix:
   - after the next APK build nothing is needed, `writeDeviceTools()` copies it to
     `$PREFIX/lib/libdroiddesk_v4l2.so`;
   - before that: `adb push app/android/app/src/main/jniLibs/arm64-v8a/libdroiddesk_v4l2.so
     tools/v4l2-shim/firefox-cam tools/v4l2-shim/out/v4l2_capture_test /data/local/tmp/`, then
     in a DroidDesk terminal `cp /data/local/tmp/libdroiddesk_v4l2.so $PREFIX/lib/ &&
     cp /data/local/tmp/firefox-cam /data/local/tmp/v4l2_capture_test ~/`. If the app cannot
     read `/data/local/tmp`, push to `/sdcard/Download/` and copy from there.
2. Without a browser (optional): `chmod +x ~/v4l2_capture_test &&
   LD_PRELOAD=$PREFIX/lib/libdroiddesk_v4l2.so:$LD_PRELOAD ~/v4l2_capture_test 3`
   → expect `RESULT: PASS`.
3. In the DroidDesk desktop terminal: `bash ~/firefox-cam` (opens https://webcamtests.com/) or
   `bash ~/firefox-cam https://webrtc.github.io/samples/src/content/getusermedia/gum/`.
   It starts a separate instance (`--no-remote`, profile `~/.mozilla-apps/camera-test`), so a
   Firefox that is already open does not swallow the URL.
4. Pages, in this order:
   - https://webrtc.github.io/samples/src/content/getusermedia/gum/ — plain getUserMedia;
     allow "DroidDesk Camera". Expect moving bars, the bouncing box, a counting frame number
     and the bottom ticker sweeping once per second.
   - https://webcamtests.com/ — name, resolution and measured fps.
   - https://webrtc.github.io/samples/src/content/getusermedia/resolution/ — VGA should run at
     30 fps, HD at 15 fps (libwebrtc's guess, see above).
   - Meet: https://meet.google.com/ → "New meeting" preview, then a 2-party call;
     `about:webrtc` shows the outgoing resolution and fps. The profile has
     `webgl.disabled=true` (camera + WebGL crashed the tab before).
5. Look for: the camera in the permission prompt / device list; a moving image; the counter
   rising ~30/s (640x480) or ~15/s (1280x720); no tab crash.
6. Logs to collect:
   - `$TMPDIR/droiddesk-v4l2.log`: one `loaded into …` line per Firefox process, then
     `open /dev/video0`, `QUERYCAP`, `S_FMT`, `S_PARM`, `REQBUFS 4 -> 4`, `STREAMON …`,
     `stats: … fps` every 10 s, `STREAMOFF`, `close … (device released)`. For a full ioctl
     trace: `DROIDDESK_V4L2_DEBUG=1 bash ~/firefox-cam`.
   - `$TMPDIR/firefox-cam-moz.log*` (MOZ_LOG `CamerasParent`, `VideoEngine`, `webrtc_trace`).
   - A tab crash: the terminal output of `firefox-cam` and `logcat`.
   - No camera at all: check that a `loaded into …/firefox` line exists (the preload reached
     the parent) and whether `open /dev/video0` appears.

### Open risks

- Firefox itself is untested: a libxul call path the shim does not hook (raw
  `syscall(SYS_openat)`, `ioctl` through another symbol) would show as "no camera" without
  `open /dev/video0` in the log. The source uses plain libc calls.
- 1280x720 runs at 15 fps in Firefox because libwebrtc guesses the fps from the width (only
  MJPEG is exempt). 720p30 would need a 1280x720 MJPEG mode (JPEG encode on the phone);
  otherwise calls use 640x480@30 or 720p15.
- The camera + WebGL tab crash is not understood; the prototype only turns WebGL off.
- fd identity: an fd closed behind the shim's back (`syscall(SYS_close)`, `close_range`)
  leaves a stale entry; if the number is reused, that fd is treated as the camera. Not on
  Firefox's capture path, but `close_range` in child-process setup is worth checking.
- A blocking DQBUF (fd without O_NONBLOCK) racing a close from another thread is not safe
  yet (Firefox uses O_NONBLOCK).
- One stream at a time per open. ~~Front/back = `/dev/video1` later~~ done in fold.81;
  Chromium under proot also needs the readdir/placeholder from §2 (a).
- ~~The Android side (Camera2 → memfd ring → `@droiddesk.camera`) replaces `dd_pattern_source`;
  timing, rotation and the foreground rules from §3 are not addressed yet.~~ Done, see below.

**Verified on device (fold.76, Galaxy Tab S10 FE):** Termux Firefox 157 with the shim finds
"DroidDesk Camera" on all test pages; Meet's green room shows the pattern at 640x480 30 fps,
720p at 15 fps, no crash (WebGL off). Capture runs in Firefox's parent process; Firefox
negotiates YU12 640x480@30 for Meet. 720p stays at 15 fps (no MJPEG mode, decided).

## Camera bridge status (real camera, 2026-10-27)

The test pattern is replaced by the phone camera. Opt-in as before: only Firefox started
through `firefox-cam` preloads the shim. Built and tested without a device (below); **not
yet run on a device**.

### Pieces

| File | What |
|---|---|
| `tools/v4l2-shim/camera_bridge_protocol.h` | Protocol v1 and the shared-memory layout (the reference for both sides). |
| `tools/v4l2-shim/frame_source_bridge.c` | Shim: the camera frame source (default). Falls back to the pattern per stream. |
| `tools/v4l2-shim/fake_camera_bridge.c` | Test server: the protocol with the moving pattern, no Android. |
| `tools/v4l2-shim/camera_jni_test/` | Host test of the app's conversion code (`camera_jni.c`). |
| `app/.../runtime/CameraBridge.kt` | App: socket server, Camera2 session, permission, notifications. |
| `app/.../cpp/camera_jni.c` | App: `ASharedMemory` ring, YUV_420_888 → I420 with rotation/crop/scale/mirror, seqlock. Library `libdroiddesk_camera.so` (CMake). |
| `app/.../view/CameraPermissionActivity.kt` | App: shows Android's CAMERA dialog for the service (no UI of its own). |
| `assets/droiddesk/firefox-cam`, `assets/droiddesk/droiddesk-camera.py` | Installed by `writeDeviceTools()` as `$PREFIX/bin/firefox-cam` and `droiddesk-camera`. |

### Protocol (v1, abstract socket `@droiddesk.camera`, text lines)

```
client: open v=1 width=640 height=480 fps=30 format=I420 [facing=front|back] [rotate=0..270] [mirror=0|1]
server: ok v=1 width=640 height=480 fps=30 format=I420 slots=3 slot_stride=462848 data_offset=4096
           size=1392640 camera=1 facing=front sensor=640x480 rotation=0      + shared memory fd (SCM_RIGHTS)
server: frame <slot> <seq> <timestamp_ns>        one per frame
server: state paused <why> | state running      camera lost / back
client: close                                    (or close the socket) camera stops at once
errors: err busy | no-permission | no-camera | bad-request | camera-error <text>
control connections: "facing front|back" -> ok (switches a running stream, remembered),
                     "status" -> ok idle | ok streaming …
```

- **Shared memory:** `ASharedMemory_create` (ashmem or memfd), header (magic `DDCM`, version,
  size, fourcc YU12, frame size, slots, slot stride, data offset, then `slot_seq[8]` and
  `slot_ts[8]`) + 3 page-aligned slots of tightly packed I420 at exactly the size the shim
  asked for. After the app maps it, `ASharedMemory_setProt(PROT_READ)` makes it read-only for
  every later mapping, so the Linux side cannot write into it.
- **No acks, seqlock per slot:** the app writes slots round-robin (`slot_seq = 0`, release
  fence, pixels, `slot_seq = seq` release) and sends `frame slot seq ts`. The shim waits on the
  socket (`poll`, ≤ 100 ms slices), copies the newest announced slot, re-reads `slot_seq` and
  drops the copy if it changed (counted as "overwritten while copying" in the log). With
  3 slots the reader has two frame periods (66 ms at 30 fps) for a ~0.5 ms copy.
- **One client at a time.** A second `open` waits up to 1.5 s (a stream that is just ending,
  as libwebrtc does STREAMOFF → STREAMON when the page changes the size), then gets `err busy`.
- **Pacing:** the app sets the AE range with the asked fps as the top (lowest bottom ≥ fps/2,
  e.g. [15, 30], so AE may slow down in the dark) and drops frames that come faster than
  0.75 of the period; the shim does the same check again.
- **Fallback in the shim:** `start()` only connects and sends `open` (it runs under the shim's
  lock, so it must not block); the answer is awaited on the producer thread. No socket, an
  `err …` answer or no answer within 35 s (the permission dialog may be up) → the test pattern
  for that stream, with one log line saying why. A stream that breaks later (app restarted)
  keeps the last frame and reconnects every 2 s. `DROIDDESK_V4L2_SOURCE=pattern` skips the
  bridge.

### Permission

- Manifest: `android.permission.CAMERA`, `android.hardware.camera.any required=false`.
- Not asked at app start (unlike the microphone): only when a Linux program first opens the
  camera. The service starts `CameraPermissionActivity` (translucent, no layout, excluded from
  recents; only Android's own dialog is visible) and waits up to 30 s for the answer.
- If Android does not let the activity start within 3 s (DroidDesk not in the foreground,
  background activity start rules) the client gets `err no-permission` and a notification
  "Kamera pro Linux — Linuxová aplikace chce použít kameru. Klepnutím povolíte DroidDesk
  přístup ke kameře, pak kameru v aplikaci zapněte znovu." opens the dialog.
- If the user declines (or declined for good): `err no-permission` and a notification that
  opens the app settings ("… v části Oprávnění povolte Fotoaparát."). The shim shows its test
  pattern, so the browser does not hang.

### Foreground / background (targetSdk 28)

- Android 9+ gives the camera only to apps in the foreground. While `DesktopActivity` (the X
  window) is visible, DroidDesk is the foreground app, which is the normal case during a call;
  `DroidDeskService` (foreground service, `dataSync` type on Android 14+) keeps the process up.
- When DroidDesk goes fully to the background, CameraService disconnects or errors the camera
  **[expected, unverified on Android 16]**. The bridge then sends `state paused <why>`, closes
  the camera and retries every 2 s; the shim keeps the last frame (the call shows a frozen
  picture), and frames resume with `state running` once DroidDesk is visible again. Whether a
  pre-R app's `dataSync` FGS keeps the camera in the background is the open question from §3;
  the behaviour above is correct either way.
- Another app taking the camera (higher priority, e.g. Android's camera app in split screen)
  is handled the same way. The privacy switch (Android 12+ quick setting) shows as
  `ERROR_CAMERA_DISABLED` or black frames **[unverified]**.
- The camera runs only while a client streams: `STREAMOFF`/close in the browser → `close` →
  `CameraDevice.close()`. `DroidDeskService.onDestroy` stops the bridge and every stream.

### Rotation and mirroring

- Clockwise rotation applied to the sensor image: front `(SENSOR_ORIENTATION − display) mod 360`,
  back `(SENSOR_ORIENTATION + display) mod 360`, display = rotation of `Display.DEFAULT_DISPLAY`
  (the phone's own screen, also under DeX) in degrees, followed with a `DisplayListener`, so
  turning the device mid-call is applied on the next frame.
- The V4L2 format stays landscape (1280x720 / 640x480). A portrait picture (phone held
  upright) is centre-cropped to the landscape aspect; the app picks a sensor size large enough
  that the crop needs no upscaling (smallest YUV size that covers the crop and runs at the
  fps, by `getOutputMinFrameDuration`).
- Not mirrored: the stream shows what others should see; browsers mirror the self-view.
- Manual correction without a rebuild: `DROIDDESK_CAMERA_ROTATE=90|180|270` (added to the
  automatic value) and `DROIDDESK_CAMERA_MIRROR=1`.

### Facing

Since fold.81 the shim fakes two devices: `/dev/video0` "DroidDesk – přední kamera"
(bus `platform:droiddesk-camera-0`, opens the bridge with `facing=front`) and `/dev/video1`
"DroidDesk – zadní kamera" (`-1`, `facing=back`), so Meet, Slack and other pages switch
cameras in their own menu. `DROIDDESK_CAMERA_FACING=back|front` forces one camera for both
devices. `droiddesk-camera back|front` still switches a running stream (closes and reopens
the camera, same V4L2 format); `droiddesk-camera` alone prints the status. The bridge serves
one stream at a time, so a page opening both devices at once gets `err busy` on the second
(test pattern). A phone with one camera serves it on both devices (the bridge falls back to
any usable camera).

### Tests (2026-10-27, no device)

- `test-glibc.sh` (Debian trixie, x86_64): PASS.
  - `camera_jni_test`: rotation 0/90/180/270, mirror, mirror + 90, centre crop, 2:1 scaling,
    NV21-style interleaved chroma (pixel stride 2, row stride ≠ width) → exact values; too
    small plane buffer rejected; seqlock word set.
  - Capture test against `fake_camera_bridge`: YU12 1280x720@15 → 15.0 fps, YU12 640x480 →
    30.0, YUYV 640x480 → 30.0 (I420→YUYV in the shim), NV12 1280x720 → 30.0; counter in the
    image rises on every frame, 0 overwritten slots; all 4 streams from the bridge (none fell
    back), including the STREAMOFF → STREAMON size changes.
  - A second client while streaming gets `err busy`.
  - Bridge killed mid-stream and restarted 2.5 s later: the shim logs "connection lost",
    "reconnected", and `ffmpeg -f v4l2` still gets its 150 frames.
  - `err no-permission` answer → test pattern, logged; no app at all → test pattern, logged.
  - Earlier checks unchanged (v4l2-ctl, ffmpeg, passthrough).
- `test-bionic.sh` (termux-docker aarch64 under qemu): PASS, including the bridge run with
  the shim stacked after `libsocket_hook.so` (memfd over SCM_RIGHTS on bionic).
- `camera_jni.c` builds with NDK r28c (API 29) and exports the five JNI functions;
  `CameraBridge.kt` + `CameraPermissionActivity.kt` compile with kotlinc 2.2.20 against
  `android-36/android.jar` (no warnings). The full APK build was not run.

### On-device test

1. Build and install the APK (the camera needs the new `libdroiddesk_camera.so`, manifest and
   `CameraBridge`; the shim alone is not enough this time). Open the desktop once:
   `writeDeviceTools()` copies `libdroiddesk_v4l2.so` and installs `firefox-cam` and
   `droiddesk-camera` into `$PREFIX/bin`.
2. In a desktop terminal: `droiddesk-camera` → `ok idle` (bridge up).
3. `firefox-cam https://webrtc.github.io/samples/src/content/getusermedia/gum/`. On the first
   camera use Android asks for the camera permission; allow it. If no dialog came (or the page
   showed the test pattern), look for the "Kamera pro Linux" notification, allow, and restart
   the camera on the page.
4. Expect the front camera, upright, not mirrored in the stream (the page's self-view may be
   mirrored by the page itself; check with text held up to the camera on
   https://webcamtests.com/ or in the remote view of a call).
5. Turn the tablet (landscape ↔ portrait): the picture stays upright (portrait = cropped).
6. `droiddesk-camera back` while the page shows video: switches to the back camera within
   about a second; `droiddesk-camera front` back again; `droiddesk-camera` shows the stream.
7. Meet green room and a 2-party call (640x480@30; 720p@15), `about:webrtc` for the outgoing
   fps. Switch to another Android app for 10 s and back: the picture freezes, then continues.
8. Start a second Firefox with its own profile
   (`FIREFOX_CAM_PROFILE=~/.mozilla-apps/camera-test2 firefox-cam`) while the first streams:
   the second shows the test pattern (busy).
9. Logs: `$TMPDIR/droiddesk-v4l2.log` (`camera bridge: ok v=1 … camera=… sensor=… rotation=…`,
   `stop after N frames (M overwritten …)`, fallback reasons), `logcat -s CameraBridge`
   (opening camera, size, fps range, rotation, paused/running), `$TMPDIR/firefox-cam-moz.log*`.
   CPU: `top -H -p $(pidof com.orailnoor.droiddesk)` — the `camera-bridge-s` thread (Linux shortens thread names to 15 characters).

### Open risks

- **Rotation sign not verified on a device.** The formulas follow Android's camera
  orientation documentation; if the picture is upside down or sideways on one of the devices,
  `DROIDDESK_CAMERA_ROTATE` corrects it at once and the formula gets fixed.
- Background behaviour of a targetSdk-28 app with a `dataSync` FGS on Android 16 (see above).
- Conversion cost: bilinear per pixel in C, no NEON/libyuv; estimated 2–4 ms per 640x480 frame
  and 6–10 ms per 720p frame (both cheap at 30/15 fps), the exact-size unrotated case is a
  plain copy **[estimate, measure on device]**.
- The fps-based size choice uses `getOutputMinFrameDuration`; some HALs report sizes that
  still run slower in a repeating request. The log line shows the chosen size and range.
- Camera + WebGL in the same Firefox tab still crashes; `firefox-cam` keeps WebGL off.
- Only Termux Firefox through `firefox-cam`. Debian Brave/Chromium under proot need the glibc
  shim build + `/dev/video0` placeholder (§2 (a), Spike B), not done.
