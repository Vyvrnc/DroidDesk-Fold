# Release verification

What each release was checked on, how, and what was **not** checked. Every APK is signed with
the same certificate; Android refuses an update signed with any other one.

Signing certificate SHA-256: `bb99b57e2d04953588f646c9a2d4d84e98b693f22ab795a70ccd4b773b13e72d`
(`apksigner verify --print-certs DroidDesk-fold-dex.apk`).

Devices:

- **Tab S10 FE**: Samsung Galaxy Tab S10 FE (SM-X620), Exynos 1580 / Xclipse 540, Android 16.
- **Fold 7**: Samsung Galaxy Z Fold 7 (SM-F966B), Snapdragon 8 Elite / Adreno 830, Android 17 (Android 16, One UI 8.5 before fold.106).

Unless stated otherwise, every release is installed over the previous one (`adb install -r`,
same as an Obtainium update) and the existing Debian, settings and Claude Code login are kept.

Automated checks before every release: `tools/glibc-native/test.sh` (pathhook and launcher
regressions in a Debian container) and `tools/check-launchers.py` (`bash -n` over the generated
launchers).

## 1.0.1-fold.106 (2026-11-01)

Changed:
- first install selects Termux's default mirror (Cloudflare CDN) unless one was chosen, skips
  Recommends and clang (80+ MB) but names the Recommends the desktop needs, Debian dpkg skips
  fsync and docs;
- the welcome screen asks for the language of the Linux apps; gettext finds the rootfs
  translations natively;
- Termux side: main-repo `mesa` (26.2.6) instead of TUR's `mesa-zink` (frozen at 22.0.5); older
  installs swap it in the background at desktop start, only when apt would remove nothing else;
  `LIBGL_KOPPER_DISABLE=true` on Adreno;
- the settings show the GPU's own name (GL_RENDERER, unwrapped from ANGLE);
- new `droiddesk-gpu-report` and `docs/gpu-test-kit.md`.

Found on Tab S10 FE: a clean install drew mirrors.pku.edu.cn at 19 kB/s (213 MB). Native Debian
programs stayed English with LANG=cs_CZ.UTF-8 (Czech under proot; Fold 7). Termux ships no
translations, so XFCE itself stays English. A clean install without Recommends had a black
desktop: xfdesktop is only a Recommends of xfce4-session (fixed, 15 packages named).

Tested on Tab S10 FE, clean install (`pm clear`, user 0):
- install 11:50 to 12:00 (under 10 minutes), packages-cf.termux.dev; the language picker
  preselected Čeština, cs_CZ.UTF-8 was generated; `ls --help` and `date` in Debian print Czech
  natively (`apt-get --help` stays English);
- the mesa-zink swap ran by itself at the next desktop start and removed only mesa-zink;
- the missing Recommends list comes from this install (installed by hand there, then the desktop
  had its wallpaper and icons again); the full clean install with the fixed list was not repeated;
- settings show "Samsung Xclipse 540";
- GPU kit, virgl off: Termux llvmpipe GL 4.6, Firefox 159 WebGL ok at 2.1 FPS. Virgl on
  (`droiddesk-native-gpu on`): Termux virgl over ANGLE/Vulkan (GL 2.1, GLES 3.1), Firefox WebGL ok
  at 56.9 FPS. Debian with `droiddesk-gpu on`: glxinfo fails with BadMatch (X_GetImage), native and
  under proot (Debian Mesa 25.0.7), not fixed. Both switched off again after the test.

Tested on Fold 7 (Android 17) with the GPU kit, from Debian:
- Debian: zink on Turnip, GL 4.6, Mesa 26.2.6; Vulkan Adreno 830;
- Termux before the swap (mesa-zink 22.0.5): zink GL 4.6; Firefox 158 WebGL 117.6 FPS, Chromium
  156 WebGL 114.8 FPS; after the swap (mesa 26.2.6): zink on Turnip GL 4.6, Firefox 114.9 FPS,
  Chromium 106.3 FPS; the desktop and scrolling looked fine;
- Chromium WebGL flickered with artefacts through kopper; same page with
  LIBGL_KOPPER_DISABLE=true: clean, 58.9 instead of 86.8 FPS;
- OrcaSlicer (Debian): GL 4.6 on zink, a model loaded and rotated;
- glmark2 varies too much between runs to compare (Debian 235 to 719 with the same settings);
- not tested: Electron (none installed), video playback (needs a person watching).

Not tested: a clean install on the Fold; Mali GPUs.

## 1.0.1-fold.105 (2026-11-01)

Note: the APK of this release still reports versionName 1.0.1-fold.104 (versionCode 105); the
version bump was skipped by a failing command chain. The code is the fold.105 change.

Tested on Fold 7 after the update: the native history directory holds all 19 sessions (including
the old proot ones) and the memory notes. The proot directory link is removed again by Claude Code
(it deletes symlinks in ~/.claude/projects); a proot session then starts its own directory, which
the next native start merges.

## 1.0.1-fold.104 (2026-10-31)

Tested on Fold 7 with a 49" monitor over DeX (3840x984, work area 3840x919), by hand from the
native Claude Code session:

- `droiddesk-zones` (script copy in ~/bin, wmctrl from `apt-get download`): layout 25|50|25 =
  0..960..2880..3840; zones 1, 2, 3, 1-2, right, left from a maximized window: frame on the zone's
  left and top edge, no overlap with the panel (xfce4-terminal rounds the size to whole cells).

Not tested: the shortcuts Super+Ctrl+1..5/Left/Right and debian-setup 20 (wmctrl, xdotool,
x11-utils) from the release APK; windows without size increments.

## 1.0.1-fold.103 (2026-10-31)

Tested on Tab S10 FE (native Claude Code session over Remote Control, builds 5291d48–2bc8ddf):

- `sudo apt-get install` / `remove` of a package and `apt-cache ... | head` from the native Bash
  tool: run under proot with fake root, no proot messages on stderr.
- Background Claude Code update under proot: 2.1.314 → 2.1.316, success stamp written, lock removed.
- debian-setup 18: ripgrep installed, `NO_AT_BRIDGE=1` in Debian login shells;
  `USE_BUILTIN_RIPGREP=0` reaches Claude; Debian `rg` sees `/etc` and `/usr`.
- `--append-system-prompt` note: seen by a fresh native `claude -p`.
- Regressions: git, codex, `/system/bin/sh`, GoogleWorkspace MCP.
- `mkdir` of a path with a trailing slash (meson install on the Fold 7 failed): container test.
- History merge of a separate native project directory: simulated in a Debian container
  (sessions, hidden files, memory notes with a name clash kept as `.native`).

Tested on Fold 7: Mesa 26.2.6-1 (new GPU asset) from its staging tree: glxinfo on zink/Turnip
(Adreno 830), glxgears, glmark2 GLX score 1916 (crashed before).

Not tested: the release APK with the trailing-slash fix on a device (container test only);
the Mesa replacement through debian-setup 19; the history merge on the Fold 7 itself (that is where it is needed; checked after the
update); a resumed session seeing the system prompt note; Wine; Electron apps.

## 1.0.1-fold.102 (2026-10-30)

Tested on Tab S10 FE (native Claude Code session over Remote Control):

- `ls /`, `find / -name debian_version`, `cd /` with relative paths, `pwd` and `/bin/pwd`,
  `cd ..` at the root.
- Python `os.listdir('/')`, `os.chdir('/')`, `os.stat(..., dir_fd=open('/'))` for `proc/self/status`
  and `etc/debian_version`.
- `find /usr -type f` (79,914 files) in 0.3 s.
- Regressions: git, codex, `/system/bin/sh`, GoogleWorkspace MCP.

Not tested: Fold 7; the release APK itself (same code as the tested build, version number only).

## 1.0.1-fold.101 (2026-10-30)

Tested on Tab S10 FE:

- Native Claude Code via `claude-tmux` with Remote Control (reconnect after `/exit` and
  `--continue`), DNS in the Bash tool, GoogleWorkspace MCP over npx.
- Node `fs.stat` on Debian paths, `fs.mkdtemp`, `fs.watch`; Python `os.system`, `os.popen`,
  `tempfile`; `pgrep` by program name.
- Static programs under proot: codex (musl), esbuild (Go) from the Termux home with Debian paths.
- `/system/bin/sh` and toybox from native programs; OrcaSlicer; git `gc` and `fsck`.

Not tested: Fold 7; Wine; Electron apps after the hook changes.

## 1.0.1-fold.100 (2026-10-30)

Tested on Tab S10 FE overnight: git, node, python, codex, tmux and Claude Code natively; Brave,
Claude Desktop (Electron mirror, Debian keyring), mpv, OrcaSlicer, SolidCraft, FuCad natively;
apt, dpkg, sudo and Wine under proot.

Not tested: Fold 7; Claude Desktop picking up an existing login.

## Template

```
## VERSION (DATE)

Tested on DEVICE (how: adb, native session, by hand):
- ...

Not tested: ...
```
