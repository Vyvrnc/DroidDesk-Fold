# Release verification

What each release was checked on, how, and what was **not** checked. Every APK is signed with
the same certificate; Android refuses an update signed with any other one.

Signing certificate SHA-256: `bb99b57e2d04953588f646c9a2d4d84e98b693f22ab795a70ccd4b773b13e72d`
(`apksigner verify --print-certs DroidDesk-fold-dex.apk`).

Devices:

- **Tab S10 FE**: Samsung Galaxy Tab S10 FE (SM-X620), Exynos 1580 / Xclipse 540, Android 16.
- **Fold 7**: Samsung Galaxy Z Fold 7 (SM-F966B), Snapdragon 8 Elite / Adreno 830, Android 16, One UI 8.5.

Unless stated otherwise, every release is installed over the previous one (`adb install -r`,
same as an Obtainium update) and the existing Debian, settings and Claude Code login are kept.

Automated checks before every release: `tools/glibc-native/test.sh` (pathhook and launcher
regressions in a Debian container) and `tools/check-launchers.py` (`bash -n` over the generated
launchers).

## 1.0.1-fold.103 (2026-10-31)

Tested on Tab S10 FE (native Claude Code session over Remote Control, builds 5291d48–2bc8ddf):

- `sudo apt-get install` / `remove` of a package and `apt-cache ... | head` from the native Bash
  tool: run under proot with fake root, no proot messages on stderr.
- Background Claude Code update under proot: 2.1.314 → 2.1.316, success stamp written, lock removed.
- debian-setup 18: ripgrep installed, `NO_AT_BRIDGE=1` in Debian login shells;
  `USE_BUILTIN_RIPGREP=0` reaches Claude; Debian `rg` sees `/etc` and `/usr`.
- `--append-system-prompt` note: seen by a fresh native `claude -p`.
- Regressions: git, codex, `/system/bin/sh`, GoogleWorkspace MCP.
- History merge of a separate native project directory: simulated in a Debian container
  (sessions, hidden files, memory notes with a name clash kept as `.native`).

Not tested: the history merge on the Fold 7 itself (that is where it is needed; checked after the
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
