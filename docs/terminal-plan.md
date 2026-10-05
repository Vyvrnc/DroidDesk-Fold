# In-app terminal (PTY) — plan (2026-10-05)

Goal: a real terminal in the DroidDesk app (no XFCE needed) for Claude Code, Codex and any
TUI (htop, vim, mc), with profiles started from the home screen.

## Choice
- Termux's `terminal-emulator` + `terminal-view` (Apache-2.0, JitPack
  `com.github.termux.termux-app:terminal-view`): VT100/xterm emulation, PTY via its own JNI
  (`libtermux.so`, createSubprocess), selection, scrollback, IME. Mature, used by Termux itself.
- A native Android `TerminalActivity` (Kotlin), started from Flutter through the existing
  method channel. A Flutter terminal (xterm.dart + flutter_pty) would need our own PTY
  plumbing and keyboard handling; not worth it.

## Profiles (intent extra `profile`)
| profile | command |
|---|---|
| shell | `$PREFIX/bin/bash -l` with getTermuxEnv() |
| debian | `start-debian` |
| claude | `claude-debian` |
| codex | `codex-debian` |
Session ends -> "[proces skončil, Enter = znovu]".

## UI
- Full-screen TerminalView, extra keys row (Esc, Tab, Ctrl, Alt, ←↑↓→, Home, End, PgUp,
  PgDn, |, ~, /) for touch; hardware keyboard works directly (DeX, Fold keyboard).
- Pinch zoom = font size (TerminalView supports scaling), saved in prefs.
- Keeps running when the activity goes to background (session owned by a small holder
  object, not the activity; DroidDeskService keeps the process alive).
- Home screen: "Terminal" opens the shell profile (replaces the line-based terminal),
  new quick actions "Claude Code" and "Codex" (shown when installed in Debian).

## Codex in Debian
- Catalog entry "Codex": Debian (installed with the desktop) + `apt-get install nodejs npm`
  + `npm install -g @openai/codex`; wrapper `codex-debian` (same binds as claude-debian,
  `cd ~/projekty/Claude || ~`). Detection: `/usr/local/bin/codex` or npm global bin in rootfs.
- Login: `codex login` (device/browser flow) in the terminal.

## Steps
1. Gradle dep, TerminalActivity + session holder + extra keys, method channel `openTerminal`.
2. codex-debian wrapper + catalog entry + detection.
3. Flutter: home screen actions.
4. Codex review of the diff (codex exec, read-only), fix findings.
5. Build, install on the tablet over adb, test: shell (htop, vim), debian, claude, codex,
   rotation, background/foreground, DeX keyboard; screenshots.
6. Release.
