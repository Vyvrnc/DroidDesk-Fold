# Backlog

Agreed items not yet scheduled for a release. Windows apps have their own plan:
`docs/windows-apps-plan.md`.

## Stability (from the Fold 7 crashes on 2026-10-20)

1. **PulseAudio watchdog.** The desktop start script starts PulseAudio once
   (`LinuxRuntime.kt`, before `exec xfce4-session`); nothing restarts it. When it dies, Debian apps
   lose sound (`127.0.0.1:4713` refused) until the desktop is restarted. DroidDeskService should
   check it every ~20 s like the X server (`pactl list short sinks` has `AAudio_sink`, with
   `timeout 2`) and restart it natively with the same arguments. It cannot be started from Debian:
   proot runs as uid 0 and PulseAudio refuses root.

2. **Keep `mapping.txt` from every build.** Two crashes on 2026-10-20 (12:27, 12:55, fold.63) were
   an uncaught `java.lang.InterruptedException` from `Thread.sleep` on a background thread, R8 frame
   `f0.k.f(SourceFile:117)` via `K0.a.run`. No `Thread.sleep` at line 117 exists in our sources, so
   it is probably a library. `tools/build-apk.sh` should copy
   `build/app/outputs/mapping/release/mapping.txt` next to the APK
   (`~/DroidDesk-build/mapping-<version>.txt`), then retrace this stack and fix the caller.

3. **Warn when the phantom process limit is on.** On the Fold 7
   `persist.sys.fflag.override.settings_enable_monitor_phantom_procs` came back as `true` although
   the developer option had been set, and Android trimmed the session's child processes every
   5 minutes ("Trimming phantom processes": XFCE, Wine, proot, Claude). At app start, read the flag
   and `max_phantom_processes`; if the limit applies, show a notice with the fix (developer option,
   or the adb commands below). Fix applied by adb on 2026-10-20, survives reboots:
   ```
   settings put global settings_enable_monitor_phantom_procs false
   device_config set_sync_disabled_for_tests persistent
   device_config put activity_manager max_phantom_processes 2147483647
   ```
   Rollback: `settings delete global settings_enable_monitor_phantom_procs`,
   `device_config delete activity_manager max_phantom_processes`,
   `device_config set_sync_disabled_for_tests none`.

4. **Why the app was cached during a build.** `dumpsys activity exit-info` shows two kills at
   11:26 and 11:51 with reason `EXCESSIVE RESOURCE USAGE / EXCESSIVE CPU USAGE`, "Caused by child
   process: excessive cpu … limit=25", importance 400 (cached). With DroidDeskService in the
   foreground the app should not be cached. Check which service type and state it had at that time
   (`dumpsys activity services com.orailnoor.droiddesk`, `dataSync` 6 h limit on Android 15+) and
   keep the app out of the cached state while a session runs.

## Other Android users and cars (ScreenMate One audit, 2026-10-22)

Android Automotive 14 on a ScreenMate One (SM6225/SD680, Adreno 610, 7.5 GB RAM). The app runs
only as user 10 (`/data/user/10/com.orailnoor.droiddesk`). Found read-only on the device by the
phone session, checked over adb root. Done so far: fold.67 CA bundle path, fold.68 hook prefix from
dladdr, 97d8a59 scrollable setup screen (D3). P = `$PREFIX`.

Priority 1, affects every non-zero user (work profiles and Secure Folder on phones too):
- **A1** `P/etc/profile` (written by the app) has literal `/data/data/com.termux/files/{usr/etc/profile.d,
  usr/etc/bash.bashrc,home/.bashrc}`, so a login shell as user 10 skips profile.d, bash.bashrc and
  ~/.bashrc. Generate it from `$PREFIX`/`$HOME`.
- **A4** relocation rewrote `files/usr` only. `P/etc/profile.d/init-termux-properties.sh` then fails
  (`mkdir /data/data: Permission denied`, `cp … /data/data/com.termux/files/home/.termux/`). Also
  relocate `files/home` → `$HOME` and `/data/data/com.termux/cache` → the real cache dir.
- **A5** `P/bin/droiddesk-relocate-shebangs`: old_prefix == new_prefix, because the relocation rewrote
  the template too, so the script does nothing. Build old_prefix at run time
  (`"/data/data/com.""termux/files/usr"`) or exclude the file from relocation. Check the Fold too.
- **E2** `debian-tmux`: `server-access` only if `getent passwd $uid` succeeds (race with
  `aid_u10_*` being added); fall back to the numeric uid or wait.
- First app terminal after setup ran the Termux bootstrap second stage again (postinst of all
  packages, py3compile errors). Probably related to A4.

Priority 2:
- **B1** PulseAudio deadlocked from the start (main thread in futex_wait, TCP 4713 backlog full,
  pactl hangs). Suspect two sinks at once: `P/etc/pulse/default.pa:120 load-module module-sles-sink`
  plus our `--load=module-aaudio-sink`. Drop sles when AAudio loads; watchdog restarts pulse when
  `pactl info` does not answer in ~3 s (see Stability 1). B2: 130 xruns on AAudio, bigger buffer on
  slow SoCs.
- **B4** PulseAudio logs "main.c: Couldn't canonicalize binary path, cannot self execute." at every
  start (realpath of /proc/self/exe fails as user 10, probably SELinux). Cosmetic, but pulse cannot
  re-exec itself.
- **A7** proot-distro binds `/data/data/<pkg>/cache` literally (`proot_distro/commands/login/
  proot_cmd.py`, `_add_termux_app_binds`), so proot warns "can't sanitize binding". Harmless;
  upstream fix: derive it from `TERMUX__PREFIX`.
- **E1** `debian-tmux` overwrites its log (`> $log`) and the XFCE terminal (`-x`) closes on an attach
  error, so failures leave nothing. Append with a timestamp and pause on error.
- **A2/A3/A6** other com.termux literals: `P/etc/bash.bashrc` (command-not-found, bash-completion),
  `P/etc/nanorc`, `P/bin/pkg` (apt cache dir), `termux-setup-storage`, `termux-reset`,
  `termux-backup`, `termux-restore`, `termux-am` (socket), `termux-wake-lock/unlock`,
  `P/etc/ssh/sshd_config` (Include, sftp Subsystem), `P/etc/ssh/ssh_config`, `profile.d/vte.csh`.
  Widen the relocation patterns (home, cache, apps); wrap or disable the com.termux service tools.

Priority 3, cars and small devices:
- **D1** panel launchers Phone/SMS only with `FEATURE_TELEPHONY`; none on `FEATURE_AUTOMOTIVE`.
- **D2** no battery genmon without a battery (it shows a fake "50 % ⚡"); hide the battery and
  hotspot setting entries there.
- **C1 (priority 1)** `/opt/mesa-kgsl/…/libvulkan_freedreno.so` needs `libxcb-keysyms.so.1`, but
  debian-setup does not install `libxcb-keysyms1`. The Vulkan loader drops the ICD ("Found no
  drivers"), zink fails and GLX ends with "couldn't find RGB GLX visual", so no GL app runs under
  gpu-env in Debian. Add the package, and in setup check `ldd` of every .so in /opt/mesa-kgsl for
  "not found". Find out why the Fold has it (pulled in by another package?). Installed by hand on
  the ScreenMate.
- **C2 (priority 1)** even with the library, zink does not work on Adreno 610: Turnip (Mesa 26.2.3)
  reports apiVersion 1.0.354 and GLX/EGL through zink fails (drisw screen, eglInitialize failed);
  llvmpipe (Debian Mesa 25.0.7) gives GL 4.5. `droiddesk-gui.sh` and `droiddesk-gpu` decide only on
  /dev/kgsl-3d0 and /opt/mesa-kgsl existing, so weaker Adrenos get a broken zink and
  `droiddesk-gpu status` says "on". Self-test at setup or first start (`glxinfo -B`/`eglinfo` under
  gpu-env, or apiVersion ≥ 1.1); on failure write /etc/droiddesk/gpu-off and report
  "Adreno XYZ: zink nefunkční, llvmpipe".
- **G1** `droiddesk-usb-tray.py` autostart (68 MB RSS, 5 s loop) only on USB_DEVICE_ATTACHED.
- **G2** a lite profile: no Thunar daemon, xfdesktop, notifyd, gvfsd-trash/metadata, gpg-agent.
- **G3** every `debian-tmux` session is its own proot + tmux server + a `sleep 5` loop.
- **F1** check that the craft-install.sh copy works as user 10 (catalog not opened yet).
  Done 2026-10-22: copied, no user-0 or com.termux paths, ALSA → pulse via /etc/asound.conf.

Found after fold.69 on the ScreenMate (setup 12 verified A1, A4, A5, B1, B3, C2, E1, E2):
- **F2** Craft menu entries go through `gpu-run`, which always sources gpu-env.sh (Turnip ICD
  and zink for GL) regardless of `gpu-off` or a failed self-test. On Adreno 610 Vulkan initialises
  (apiVersion 1.0.354) but GL through zink is broken, so GTK/webview dialogs in Craft apps may
  fail. With gpu-off or a failed self-test: keep the Vulkan ICD only, GL stays on llvmpipe.
- **F3** no Vulkan fallback in Debian (no `mesa-vulkan-drivers`/lavapipe, no
  /usr/share/vulkan/icd.d): when Turnip fails, wgpu apps have no Vulkan at all. Install lavapipe
  with the first Craft app, or test before installing. F4: wgpu on Vulkan 1.0 untested (no Craft
  app installed on the ScreenMate yet; photocraft or pdfcraft is the smallest test).
- **Self-test detail** `/usr/local/sbin/droiddesk-gpu-selftest` reports the last non-empty glxinfo
  line ("name of display: :0") when there is no renderer; take the first
  `error|fail|couldn` line instead.
- A2, A3, A6 (sshd_config), A7, D1, D2, G1 still open as listed above.
- **Debian zink works where ours does not** (root-09 test, 2026-10-22): with only the Turnip ICD
  (no /opt/mesa-kgsl in LD_LIBRARY_PATH), Debian's Mesa 25.0.7 puts GL on zink by itself and it
  renders on Adreno 610 (GL 4.0), while zink from /opt/mesa-kgsl 26.2.3 fails. glxgears was not
  faster than llvmpipe there (253–261 vs 293–335 FPS, vsync on). Option for later: when the
  self-test fails with our Mesa, try Debian's zink before falling back to llvmpipe.
  fold.70 gpu-run with gpu-off keeps GL on llvmpipe (LIBGL_ALWAYS_SOFTWARE=1, GALLIUM_DRIVER=llvmpipe).
  A7 is fixed in fold.70 by patching proot_cmd.py at every app start.

Craft test round on the ScreenMate with fold.70 (root-09, 2026-10-22):
- Works: SoundCraft plays through pulse (sink-input on AAudio_sink); PdfCraft and WordCraft on
  Turnip Vulkan 1.0 (wgpu, window in ~0.6 s) and on lavapipe only (~1.3 s to ready, usable);
  WordCraft .docx and .odt (via Export) round trip with Czech diacritics. 5 min with desktop,
  Claude and three apps: nothing died, ~4 GB available, swap unchanged (1.2/4 GB). RSS:
  SoundCraft 166 MB, WordCraft 163 MB, PdfCraft 132 MB (Turnip) / 173 MB (lavapipe), Claude
  390–450 MB.
- **F6** Craft apps want a system file dialog ("install xdg-desktop-portal-gtk or zenity"), else
  their own dialog starts in /root every time. Add `zenity` (small) with the first Craft app.
- **F7** SoundCraft keeps its sink-input open and the AAudio stream RUNNING after Stop (~10 % of
  a core idle). Upstream issue, not ours.
- Typing Czech through `xdotool type` dropped characters; check with a real keyboard before
  blaming WordCraft. Paste works.
- `/proc/loadavg` in proot is a static file from proot-distro's sysdata: nothing in Debian
  should rely on it.

## Display scaling (tablet, 2026-10-26)

- **Windows do not follow a rotation into the work area** (Fold 7, fold.82): after turning the
  folded phone to portrait (1968×2184), Firefox stayed at 0,204 1968x1952 (bottom ≈2156 px)
  while the panel starts at ≈2053 px, so Meet's control bar was hidden behind the panel. xfwm4
  does not re-place unmaximized windows on a RandR change; options: re-fit windows that
  overlap the panel after the screen change (wmctrl/xdotool in the display-change hook), or
  open browsers maximized on phones.
- **Sharp zoom.** The zoom button (Wide/Phone/Monitor 125–200 %) lowers the X resolution and
  stretches it (`DisplayProfile.kt`: X resolution = window / scale, `displayStretch`), so anything
  above 100 % is blurry; 125–175 % even more (non-integer stretch). Workaround that works:
  DroidDesk zoom at 100 % and XFCE Appearance → Window Scaling 2× (integer only), in-between
  sizes through Appearance → Fonts → Custom DPI (120 ≈ 125 %, 144 ≈ 150 %, 168 ≈ 175 %).
  Plan: X server always native; zoom maps to xsettings (`/Gdk/WindowScalingFactor` 2 for 200 %,
  `/Xft/DPI` = 96 × zoom for the steps between, bigger cursor, QT_SCALE_FACTOR / winit scale for
  Qt and Craft apps), per screen (inner, cover, DeX). Keep today's stretched mode as a fast
  option for weak devices (ScreenMate One).
- **Window decorations at 2×.** xfwm4 ignores Window Scaling; its themes are fixed-size pixmaps.
  Termux's fluent-gtk-theme ships only Fluent, Fluent-Dark, Fluent-Light and the -compact
  variants, no -hdpi/-xhdpi. `Default-xhdpi` helps a little. Build Fluent-Dark-xhdpi (and -hdpi)
  xfwm4 variants from the upstream SVG assets (vinceliuice/Fluent-gtk-theme) and switch to them
  together with the 200 % / 150 % zoom.
- **Czech XFCE is not possible as is**: Termux packages ship no translations
  (`$PREFIX/share/locale/cs` does not exist), so LANG/LANGUAGE change nothing on the native
  desktop. Debian apps already follow the Android language.

## Electron apps in Debian (tablet, 2026-10-26)

- Claude desktop (claude-desktop 3.6556.2 arm64) does nothing from the menu: as root in proot
  Electron stops with "Running as root without --no-sandbox is not supported"; with
  `--no-sandbox` only it exits 1 without a log (GPU process on virgl, Tab S10 FE); with
  `--no-sandbox --disable-gpu` it runs. Tablet workaround: `/usr/local/bin/claude-desktop` →
  `exec /usr/bin/claude-desktop --no-sandbox --disable-gpu "$@"`. Same will hit VS Code,
  Obsidian, Slack desktop etc. Plan: droiddesk-menu-sync detects Electron apps (a
  `chrome-sandbox` or `resources/app.asar` next to the binary) and runs them through a small
  `electron-run` wrapper that adds `--no-sandbox`, plus `--disable-gpu` where hardware GL is off
  or the GPU is virgl; check `--ozone-platform=x11` and Adreno (zink) separately.
- **OAuth callbacks of Debian apps** (Claude desktop Google login, tablet 2026-10-26): the browser
  (Termux Firefox, default for http/https) opens `claude://…`, but nothing on the Termux side
  handles `x-scheme-handler/claude`. droiddesk-menu-sync writes
  `debian-com.anthropic.Claude.desktop` with that MimeType, yet Termux's mimeinfo.cache /
  mimeapps.list never learn it. Tablet workaround: `x-scheme-handler/claude=debian-com.anthropic.Claude.desktop`
  in Termux `~/.config/mimeapps.list` [Default Applications]. Fix: menu-sync runs
  `update-desktop-database` on the Termux applications dir and copies every
  `x-scheme-handler/*` of the synced Debian entries into mimeapps.list (without overriding a
  default the user set). Applies to every Debian app with OAuth over a custom scheme.
- **Secret storage for Debian apps** (Claude desktop, tablet): Electron safeStorage reports
  "No system keyring"; Debian cannot use the Termux session bus
  (`DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-session` → "Did not receive a reply", likely
  auth/uid across proot). Tablet workaround in `/usr/local/bin/claude-desktop`:
  `dbus-run-session` → `gnome-keyring-daemon --daemonize --unlock --components=secrets` (login
  keyring, empty password, ~/.local/share/keyrings) → app with `--password-store=gnome-libsecret`.
  Generic fix: a Debian session bus plus gnome-keyring started once per Debian session (shared by
  all apps), and the Electron wrapper passes `--password-store=gnome-libsecret`. Decide whether
  an empty-password keyring is acceptable (it is plain on disk) or unlock it with a key from
  Android Keystore.
- **xdg-open of http(s) from Debian** (Electron sign-in): droiddesk-open → android-app-launcher
  in mode xdg opened Firefox, but XFCE also showed "Failed to execute default Web Browser …
  /data/user/0/com.orailnoor.droiddesk/files/usr/bin/firefox". Looks like a second launch
  attempt (exo-open) or a Debian env leaking into the native side; check how mode xdg handles
  http(s) and which process shows the dialog.
  **Chosen direction (Matěj, 2026-10-26): unlock with a key from Android Keystore.** Design:
  1. App: AES-256-GCM key in Android Keystore (non-exportable, `setUnlockedDeviceRequired(true)`),
     a random 32-byte keyring password generated once, stored only encrypted (prefs/file).
  2. Debian session host (like debian-tmux's): `dbus-daemon --session` on
     `unix:path=/tmp/dbus-debian` plus `gnome-keyring-daemon --daemonize --unlock
     --components=secrets`; the app decrypts the password and writes it to the host's stdin
     (`--unlock` reads stdin) — never argv, env or a file. Debian launchers export
     `DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-debian`.
  3. The Electron wrapper adds `--password-store=gnome-libsecret`.
  4. Migration: an existing empty-password keyring (tablet) is replaced; stored secrets are lost
     once (sign in again), or re-encrypted if `ChangeWithMasterPassword` works.
  5. Lost key (app reinstalled, Keystore wiped): keyring reset with a notice, secrets lost.
  Protects the keyring file against copies of the app data; not against processes running as
  the app while the phone is unlocked (same as on a Linux desktop). Open: what removing the
  screen lock does to an `UnlockedDeviceRequired` key (unverified); before the first unlock after
  a reboot the keyring stays locked by design. Effort ~1–1.5 days incl. tests on Fold and tablet.

### fold.72: implemented, untested on devices (2026-10-26)

- **Electron**: `/usr/local/bin/electron-run` (debian-setup 14) adds `--no-sandbox
  --password-store=gnome-libsecret`, plus `--disable-gpu` with `/etc/droiddesk/gpu-off` or without
  `/dev/kgsl-3d0`; `DROIDDESK_ELECTRON_GPU=0|1` overrides. droiddesk-menu-sync routes an entry
  through it when its program (after `env`, `VAR=…`, `gpu-run`) is a known Electron app, has
  `chrome-sandbox` or `resources/app.asar` next to the resolved binary, or is a small launcher
  script mentioning Electron; such entries carry `X-DroidDesk-Electron=true`. Chosen in menu-sync,
  not as `/usr/local/share/applications` overrides like Craft: detection is generic, menu-sync
  reads that directory itself, and the URL-scheme handler entries go through the same Exec. A
  Debian terminal needs `electron-run claude-desktop`. Adreno with the GPU on is untested.
- **URL schemes**: menu-sync adds `x-scheme-handler/<s>=debian-….desktop` to Termux
  `~/.config/mimeapps.list` [Default Applications] only while the scheme has no default, records
  its lines in `~/.config/droiddesk/debian-scheme-handlers` and removes them with the entry (unless
  the user changed them). http, https and file are skipped. Debian's `update-desktop-database`
  runs on the Termux applications dir when installed (Termux's cannot run inside proot).
- **Keyring**: `KeyringSecret.kt` (AndroidKeyStore AES-256-GCM, `setUnlockedDeviceRequired`, falls
  back to a key without it if refused; password in prefs `droiddesk_keyring` as IV + ciphertext).
  Host `assets/droiddesk/droiddesk-session-bus`, copied into `/usr/local/sbin` and started through
  debian-run at desktop start, terminal start and after debian-setup; stopped with Stop Server.
  `/etc/profile.d/droiddesk-dbus.sh` switches login shells to `/tmp/dbus-debian` while the socket
  exists. `dbus-daemon --nofork` (not `--fork`) and `gnome-keyring-daemon --foreground` keep
  their pids known to the host, which is the first process of its proot. Old keyring without the
  marker `.droiddesk-keystore` (tablet), a lost key (`--reset`) or a keyring that does not unlock
  → archived as `login.keyring.bak-<date>`, new one created, Czech notice via notify-send.
  Logs: `tmp/droiddesk-debian-bus.log`. Tested in a Debian trixie container (fresh, idempotent,
  restart, wrong password, reset, migration); not yet in proot on a device.
  The tablet's hand-made `/usr/local/bin/claude-desktop` (it contains `gnome-keyring-daemon`) is
  archived by debian-setup 14 as `claude-desktop.droiddesk-bak-<date>`.
- **Double browser launch** (not fixed, cause not proven): the only second attempt in the code is
  `openWithLinuxApp` (AndroidAppBridge mode `xdg`): `gio open URL || exec exo-open URL`. The
  dialog text is exo's WebBrowser helper, so gio open returned non-zero. Either gio launched
  Firefox and still failed, or gio failed (no `x-scheme-handler/https` default; for http GLib may
  also try to resolve the URL through gvfs) and exo's helper launched Firefox but reported a
  failure because the started `firefox` process exited non-zero after handing the URL to the
  running instance. `tmp/droiddesk-open.log` now records both exit codes (scheme only); with them:
  gio ≠ 0 and Firefox opened by gio → drop the exo fallback for URLs; gio ≠ 0 with no window
  from gio → set `x-scheme-handler/http(s)` defaults to Firefox in Termux mimeapps.list and use
  gio only.

## Windows-like desktop as the default (Matěj, tablet, 2026-10-26)

Matěj set XFCE up like Windows on the Tab S10 FE and wants it on the Fold and as the DroidDesk
default. Copy of the config: tablet `/sdcard/Download/droiddesk-layout-2026-10-26/`
(xfce4-panel.xml, xfwm4.xml, xsettings.xml, xfce4-desktop.xml).
- One bottom panel: `p=8` (bottom centre), length 100 %, locked, size 60, icon-size 36, black
  background, dark mode. Order: applicationsmenu (icon only) → launchers terminal / files /
  browser → tasklist (no labels, grouping 1) → separator (expand) → systray (square icons) →
  genmon droiddesk-battery → clock (%H:%M) → showdesktop at the far right. Orphan plugins
  5/8/11/14 must not go into the default.
- xfwm4: placement_mode=center, placement_ratio=20, button_layout `O|SHMC`, title left,
  tile_on_move, snap_to_border.
- Look: Fluent-Dark / Fluent-dark icons / xfwm4 Fluent-Dark, Sans 11, Adwaita cursor 50.
- Default: write x/y from the resolution (p=8 + 100 % is enough); smaller panel on the Fold cover
  screen and phones (size 60 / icons 36 is too big there), ties in with the sharp-scaling item.
  Apply only to new installs or on request ("Apply Windows layout" in Desktop tools), never over
  an existing user layout. No battery genmon without a battery (D2).

## Web apps (Slack etc., tablet, 2026-10-26)

Slack as a separate app on the tablet: Termux Firefox with its own profile (~/.mozilla-apps/slack,
user.js + chrome/userChrome.css hide tabs and nav bar), `--no-remote --class Slack --name Slack`,
launcher `slack-web.desktop`. Firefox sets its own `_NET_WM_ICON`, so the tasklist showed the
Firefox icon (profile `chrome/icons/default/main-window.png` is ignored). Being tested: 
`x11-seticon.py CLASS ICON.png [PID]` (python3 + ctypes on libX11, minimal PNG decoder, sets
`_NET_WM_ICON` on windows of that WM_CLASS; re-applies for 60 s because Firefox overwrites it
after map) started by a `slack-app` wrapper. Plan: a generic "web app" helper in DroidDesk
(name, URL, icon → profile, .desktop, icon fix), also for Teams, Outlook, Meet; maybe a small
UI in Desktop tools.

## GPU for the native desktop without Adreno (tablet, 2026-10-27)

**Status: experimental opt-in, implemented, untested on devices.** Default stays llvmpipe.
`droiddesk-native-gpu on|off|status` (asset `droiddesk/droiddesk-native-gpu`, installed to
`$PREFIX/bin`). `on` refuses on Adreno (Turnip + Zink already), otherwise installs Termux's
main-repo `mesa` instead of `mesa-zink` (simulated first, stops if apt would remove anything else;
the swap is recorded in `$PREFIX/etc/droiddesk/native-gpu-mesa-swapped`, `off` never swaps back),
plus `virglrenderer-android` and `angle-android`, and writes
`$PREFIX/etc/droiddesk/native-gpu-virgl`. The app's virgl server (the Debian one) then also starts
for this marker; the native session gets `GALLIUM_DRIVER=virpipe` without
`LIBGL_ALWAYS_SOFTWARE`/llvmpipe override (`LinuxRuntime.nativeVirglActive`: marker, no Adreno,
`mesa` installed, server socket present; session start waits up to 5 s for the socket).

Findings (Galaxy Tab S10 FE, Xclipse, Android 16):
- Termux's Mesa ignores `VTEST_SOCKET_NAME` and connects to `$PREFIX/tmp/.virgl_test`; the server
  binds `$TMPDIR/.virgl_test` (files/tmp). The app now links `$PREFIX/tmp/.virgl_test` to it when
  the server starts (both paths are real, well under the 107-byte `sun_path`).
- `mesa-zink` 22.0.5 (TUR) is too old for virglrenderer-android 1.3.0: capset size mismatch, stack
  corruption abort. Termux's `mesa` 26.2.5 works: glxgears 139–149 FPS, renderer
  "virgl (ANGLE (Samsung… Vulkan))". Adreno keeps `mesa-zink` (zink without kopper, no flicker).
- Firefox with HW WebRender over virgl vs llvmpipe: scrolling ~50 % less CPU, YouTube ~23 % less.
  Firefox chooses WebRender HW by itself when GL works; no prefs are forced.

**Known crash:** YouTube (ambient mode canvas readback) intermittently kills Firefox: the client
dies after a `TRANSFER_GET2` readback, the server log (`$TMPDIR/virgl-server.log`) repeats
`virgl_fence_set_fd: failed err=-9` / `failed to export fence sync object`. Workaround: turn off
YouTube's ambient mode, or start Firefox with `GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=true`.
Next: find why the fence export fails (ANGLE's EGL sync on Xclipse, `EGL_ANDROID_native_fence_sync`)
before this can become a default.

Night test 2026-10-28 (Tab S10 FE, Firefox with gfx.webrender.all on virpipe, Mesa 26.2.5,
20 s YouTube starts): 13 of 16 starts failed at t≈7–10 s, the player's start, mostly
SIGSEGV (NULL deref, si_addr 0x4) in Firefox's Renderer thread inside libgallium under
libEGL_mesa (EGL entry point on the drisw/X11 path; DRI3 is unavailable), sometimes as
WebRender "OutOfMemory" → software fallback; often after YouTube's 300x150 4x MSAA WebGL
FBO comes back incomplete (0x8cd6). Not a leak (virgl child fd/RSS/dmabuf flat, ≥1.9 GB
free) and not a static caps gap (the same FBO formats/samples are complete in a standalone
EGL test). `webgl.msaa-samples=0` does not help. With `webgl.disabled=true`: 0 of 26
failures, 5 min runs stable, so the hint recommends that pair. Video still costs about the
same CPU as llvmpipe (Firefox ~400 % + virgl ~78 %); the gain is scrolling and UI.
Next: Termux Mesa with debug info for a symbolized backtrace of the EGL crash.
Also seen once in 29 starts: Firefox hangs at start without a window (3 % CPU, only the
startup Mesa lines in its log), probably glxtest or the virgl connect; unexplained.

Other limits: if the server dies, apps started from the running XFCE keep `virpipe` until the
desktop restarts (the app removes the socket, so new terminals and the next session use llvmpipe).

## Path audit of the Termux prefix (tablet, night 2026-10-27/28, fold.81)

Breaks now:
- **Samba**: 96 `lib*-private-samba.so` live only in `$PREFIX/lib/samba` and are found through
  RUNPATH `/data/data/com.termux/…/lib/samba`, which the linker does not rewrite: testparm,
  smbclient, smbd, net, rpcclient, ldb* fail with CANNOT LINK EXECUTABLE. Fix like pulseaudio:
  symlinks into `$PREFIX/lib` (or a RUNPATH patch with patchelf) after install.
- **graphviz** `gvpack`: `libgvplugin_neato_layout.so` only in `$PREFIX/lib/graphviz`, same fix.
- **termux-tools**: termux-setup-storage, termux-wake-lock, termux-reload-settings and
  termux-api-start send intents to `com.termux`; termux-backup/restore `tar -C
  /data/data/com.termux/files` (chdir is not hooked). Hide or replace with DroidDesk versions.
- xkeystone needs nickle (not installed, the same in upstream Termux), ignore.

Only works with the hook (fine inside the session, low priority):
- CPAN scripts outside dpkg (mimetype, mimeopen) keep `#!/data/data/com.termux/…/perl`;
  relocate-shebangs only walks dpkg-owned files.
- Config files with com.termux paths: nanorc, bash.bashrc, sshd_config, ssh_config, my.cnf,
  gtk immodules.cache, X11 Compose includes.

Cosmetic: 898 `.pc` files with the com.termux prefix; 19 broken symlinks (16 Fluent icons, 2
termux-services svlogger, the tmp virgl link while no server runs). Debian `/etc` clean; no
PT_INTERP points at com.termux; all 86 Termux `.desktop` entries resolve.

Camera: without the Android CAMERA permission the bridge waits ~30 s for the permission dialog
before the shim shows the test pattern. When the permission is permanently denied (no dialog
possible), answer `err no-permission` at once.
