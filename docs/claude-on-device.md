# Claude Code on the device: running Termux commands from Debian

Claude Code runs in Debian (`claude-debian`, proot). Termux tools live outside it; this is
what works (found on the Fold, 2026-10-03/04).

Paths: inside proot the app's data is only reachable as
`/data/user/0/com.orailnoor.droiddesk/files/{usr,home}`; `/data/data/com.orailnoor.droiddesk`
does not exist there.

## Termux ELF binaries directly

```sh
P=/data/user/0/com.orailnoor.droiddesk/files/usr
env LD_LIBRARY_PATH=$P/lib DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-session DISPLAY=:0 \
    PATH=$P/bin $P/bin/xfconf-query -c xsettings -p /Net/ThemeName
```

Works for ELF programs (python3, xfconf-query, gdbus, readelf). Not for:
- scripts with `#!/system/bin/sh` (Termux's `dpkg`, `pkg`, wrappers): "Operation not permitted";
- `pkg` itself: "Cannot run pkg as root" (proot is a fake root);
- GTK programs: the socket hook (`LD_PRELOAD`) does not work inside proot.

Never set `LD_LIBRARY_PATH` globally in the Debian shell.

## Really native: through the XFCE terminal

1. Write a script into Termux's home (writable from Debian), e.g. `~/skript.sh` there:
   `exec > ~/out.txt 2>&1`, the commands, `echo KONEC` at the end.
2. With `xdotool` in Debian (`DISPLAY=:0`): activate the XFCE terminal window, check
   `[ "$(xdotool getactivewindow getwindowname)" = "Terminal" ]`, type `bash ~/skript.sh` + Enter.
3. Wait for `KONEC` in `out.txt` and read it.

Used for pkg install, tumblerd, gio mount, logcat, dlopen with the hook.

## Traps

- `ls` from proot on Termux directories can be incomplete (tumbler plugins looked missing);
  verify natively before concluding a file is missing.
- `pkill -f` / `pgrep -f` with a pattern also match the calling shell; kill by PID.
- Claude Code 2.1.289 in proot: "Cross-session messaging is off … user namespace without a uid
  mapping". `claude-debian` (and the Claude Code menu entry) passes
  `--messaging-socket-path ~/.cache/claude-msg/<pid>.sock` (directory mode 0700, required) since fold.39; a plain `claude` in Debian
  needs it by hand. Remote Control works either way.
