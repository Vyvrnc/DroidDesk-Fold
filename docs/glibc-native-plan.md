# Debian (glibc) programs without proot

Status: spike done on the Galaxy Tab S10 FE (night 2026-10-29/30, fold.89/90). Full notes on the
tablet in `~/glibc-spike/FINDINGS.md`; this file is the design that came out of it. Step 1 now
has a Termux-side launcher; step 2 adds native-first routing and background setup. The current device baseline (Android 16) passes setup/run,
TracerPid 0, git/grep/tar/python/node/npm and DNS. The five fixes below still need device validation.

## Why

proot runs every Debian program under ptrace: each system call stops the program twice. Work
made of system calls (files, processes, starting programs) is many times slower than native and
burns CPU in the tracer; pure computation is not affected.

| Workload (tablet) | proot | native | |
|---|---|---|---|
| `du -s /usr` (7 GB) | 28 s | 0.28 s | ~100× |
| `find /usr -type f` (79k files) | 12–17 s | 0.20 s | ~65× |
| `grep -r` (7.7k files) | 6.0 s | 0.20 s | 30× |
| `tar` 127 MB | 9–13 s | 0.29 s | ~30× |
| `git status` (5.4k files) | 3–7 s | 0.31 s | ~22× |
| gcc hello.c | 310 ms | 69 ms | 4.5× |
| `npm` (start) | 3.2 s | 0.56 s | 6× |
| `python3 -c pass` ×20 | 1.35 s | 0.71 s | 1.9× |
| `xz -1` 40 MB (CPU only) | 3.34 s | 3.39 s | same |
| Brave: to a window | 5.1 s | 1.2 s | 4× |

CPU time including the proot tracer (the energy proxy, the tablet was charging): git/grep/tar/
python 6.7–10.8 CPU-s under proot against 0.16–0.38 native; Brave, 85 s with 60 s of scrolling,
102–106 CPU-s against 68 (about 35 % less, the tracer alone took 21–22).

## How it works (verified on copies)

1. **Patched loader and libc.** Android's seccomp kills a process (SIGSYS) on 114 system calls
   that exist on arm64 (list: `seccomp-blocked.txt` on the tablet). glibc calls
   `set_robust_list` and `rseq` in ld.so before any constructor, so a preloaded SIGSYS handler is
   too late. Copies of `ld-linux-aarch64.so.1` and `libc.so.6` have those `svc` instructions
   replaced by `movn x0,#37` (-ENOSYS); `GLIBC_TUNABLES=glibc.pthread.rseq=0`. libuv (node)
   needs the same for `io_uring_setup` (`UV_USE_IO_URING=0` is not enough). The patch is found
   by pattern, so it can be redone after a glibc update in Debian.
2. **PT_INTERP + RPATH on copies of the programs** (patchelf, in Termux): the kernel starts the
   program through the patched loader itself, so `/proc/self/exe` is the real program and
   Chromium/Brave zygote and renderers, crashpad, gcc → cc1 work without an exec wrapper.
   Order matters: first `patchelf --force-rpath --set-rpath "<plib>:…"`, then
   `--set-interpreter <plib>/ld.so` in a second call; both at once or the other way round made
   non-PIE binaries (cc1) crash (the new PHDR segment overlapped .text). The RPATH must start
   with the patched libc. `LD_LIBRARY_PATH` is not usable: it breaks the bionic Termux tools in
   the same shell.
3. **Data paths.** Environment variables cover locale (`LOCPATH`), time zones (`TZDIR`, `TZ`),
   fonts (`FONTCONFIG_SYSROOT`, `FONTCONFIG_FILE`), TLS (`SSL_CERT_FILE`), perl (`PERL5LIB`) and
   gconv. DNS cannot: libc reads `/etc/resolv.conf` itself, so the patched libc has that string
   replaced by `/proc/self/fd/63` (same length) and the launcher opens the file as fd 63. The
   rest goes through a small glibc path hook (`pathhook.so`, loaded with
   `ld.so --preload`): open/stat/access/opendir/readlink/realpath/dlopen … of `/usr /etc /var
   /opt /lib /bin /sbin` → the rootfs, `/tmp`, `/dev/shm` → Termux's tmp. node, npm, fc-match and
   perl XS run with it.
4. **Launcher with a clean environment.** Termux variables (GIO_MODULE_DIR …) made glibc
   programs load bionic modules ("invalid ELF header"). Brave also needs
   `--disable-dev-shm-usage` with TMPDIR and `DISPLAY` set to the full path of the X socket.

## What stays under proot

Blocked calls that matter: the set*uid/gid family except setresuid (apt drops to `_apt`, sudo),
`accept` (accept4 works; servers using plain accept), SysV IPC (shm/sem/msg), POSIX mqueues,
keyctl, io_uring, futex2, the mount API. So `apt`, `sudo`, and unsupported servers stay on proot.
The libc patch also replaces inline immediate-number syscall sites for setregid (143),
setgid (144), setreuid (145), setuid (146), setresgid (149), setfsuid (151), setfsgid (152)
and setgroups (159) with `movn x0,#37` (-ENOSYS). setresuid (147) remains allowed and unpatched.
The native tmux client on Tab S10 FE (fold.95) was killed by SIGSYS at setfsuid; tmux ignores
that return value, so no success emulation is needed. Every patched site is reported.
The loader still requires its original robust-list/rseq sites; libc must also match at
least one original site before any new set*id sites count as success.

This is not complete setxid emulation: in multithreaded glibc, setuid/setgid and related
calls broadcast to other threads. Those threads issue a syscall with a variable number,
which the immediate `movz x8,#NR ... svc #0` patch cannot reach. Such callers can still get
SIGSYS (or fail glibc setxid consistency checks) and must use proot. Direct syscalls outside
the patched libc are also unaffected.

## How to use (step 1)

Build the shipped arm64 hook with `bash tools/glibc-native/build.sh` before building the app.
The app installs the launcher and its assets at startup. Step 2 also runs setup in the
background after Debian setup/repair is ready, unless native routing is switched off.
From a **Termux-side terminal outside proot**, with Debian already installed:

```sh
droiddesk-glibc-native setup
droiddesk-glibc-native selftest
droiddesk-glibc-native run git --version
droiddesk-glibc-native run /bin/sh -c 'python3 -c "import ssl"; git --version'
droiddesk-glibc-native claude
```

`run` is the explicit native entry point. Debian, Claude and Codex launchers now try native
first and use proot when preflight declines it (details below). Native children inherit the path hook, so a tool chain must start through
this launcher from Termux; launching it under proot still traces its descendants. The hook
maps Debian system paths into the rootfs and temporary paths into Termux tmp; HOME and shared
Android paths stay unchanged. `run` inherits the Termux HOME unless explicitly overridden
(for example `HOME=/physical/path droiddesk-glibc-native run ...`). `claude` uses the existing
Debian root installation and defaults to its physical rootfs `root` HOME, matching the proot
launcher. It warns in Czech that both sessions share `~/.claude`. Use
`droiddesk-glibc-native claude --home /home/test ...` or `DD_CLAUDE_HOME=/physical/path` to
select another configuration HOME; `/root` and `/home` arguments map into the rootfs.
Installation lookup remains in the default Debian home.
`getcwd` and `realpath` remove the rootfs prefix only for mapped system directories, preserving
the physical HOME. Mapping covers interposable libc calls; direct syscalls, static programs,
libc-internal calls are outside the path wrappers. NSS is handled separately: getpwnam,
getpwuid, getgrnam, getgrgid and their `_r` variants parse `$DD_ROOTFS/etc/passwd` and
`/etc/group` with glibc file parsers and the real fopen, bypassing libc-internal NSS opens.
getpwent/setpwent/endpwent and getgrent/setgrent/endgrent use independent per-thread streams;
non-`_r` lookup results use per-thread buffers, grown on ERANGE and released at thread exit.
getgrouplist includes the primary group once and deduplicates supplementary gids, reporting
the required capacity when too small. initgroups succeeds without changing Android-owned
supplementary groups or calling blocked setgroups. With DD_ROOTFS unset these functions
delegate to libc. This files-only view does not consult LDAP, other NSS modules or shadow.
For mapped paths (including physical rootfs paths),
the hook walks components and resolves absolute links relative to the Debian root, relative
links from their parent directory, with a 40-link limit. Final links are preserved for
lstat/readlink/unlink/rename/symlink creation and other no-follow operations. Buffers belong
to each invocation, so concurrent and two-path calls cannot overwrite one another.
This is path compatibility, not a filesystem isolation boundary.

Run `setup` again after Debian upgrades. It hashes the source loader, libc, optional libuv
and shipped helpers, and rebuilds patched copies only when they change. Unknown instruction
patterns fail with an error before replacing the current copies. Nothing installs packages
automatically: setup needs Termux `python3` (`pkg install python`). On every setup it also
refreshes absolute library symlinks from Debian `lib/aarch64-linux-gnu` and
`usr/lib/aarch64-linux-gnu` into `N/lib`, resolving alternatives inside the rootfs. Both
`DD_LIBPATH` and loader `--library-path` put `N/lib` immediately after `plib`.

Because LOCPATH disables locale-archive, setup runs Debian localedef natively with
`--no-archive` to create directory locales in `N/locale`: current LANG, C.UTF-8,
cs_CZ.UTF-8 and en_US.UTF-8 when their sources/charmap are installed. Existing directory
locales are reused; the rootfs C.utf8 directory is linked as a C.UTF-8 fallback. LOCPATH
searches `N/locale` then the rootfs locale directory. A missing requested LANG silently
falls back to C.UTF-8; C and POSIX remain valid. An explicit TZ wins. When TZ is empty,
Android `getprop persist.sys.timezone` is cached for one hour in `N/timezone.cache`
(epoch seconds and timezone on separate lines). Reading a fresh cache uses only Bash
builtins. Empty results are cached too; a missing/invalid/expired record is refreshed
and published by renaming a private file. Cache write failures do not block a launch.

For a binary that re-executes `/proc/self/exe`, install Termux patchelf with `pkg install
patchelf`, then run `droiddesk-glibc-native patch /usr/bin/PROGRAM`. It prints the path of a
copy under `$PREFIX/opt/glibc-native/bin`; launch that path with `run`. RPATH is patched before
the interpreter in separate calls. A DT_NEEDED entry keeps the hook loaded on re-exec without
leaking LD_PRELOAD into Termux tools. Existing `$ORIGIN` entries point back to the source
directory. Re-run `patch` after upgrading that program. Claude Code single-file Bun ELFs
must stay original: patchelf can move the first LOAD segment and break absolute references.
`claude` resolves Debian `~/.local/bin/claude` to the versions ELF and dispatches the original
through the patched loader without modifying it. The npm installation runs Debian node
with `cli.js`.

Chromium/Brave/Electron need their data next to `/proc/self/exe`. Use:

```sh
droiddesk-glibc-native app /opt/brave.com/brave brave
# Run the printed command, for example:
droiddesk-glibc-native run "$PREFIX/opt/glibc-native/apps/brave/brave" --no-sandbox --disable-dev-shm-usage --password-store=basic
```

`patch /opt/brave.com/brave` also creates the mirror. Each source directory gets
`N/apps/<directory-name>`: data files and subdirectories are symlinked to the source;
ELF executables with a standard glibc interpreter (including chrome_crashpad_handler) are
patched copies under their original names. RPATH is patched first, interpreter second in
separate calls, then the hook is added as DT_NEEDED. Mirror `$ORIGIN` entries stay local.
The launcher supplies DISPLAY and TMPDIR. Directory names must be unique within `N/apps`;
rerun after upgrades (removed source entries can leave unused mirror entries).

DNS uses inherited fd 63; programs that close it need separate validation. The clean native
environment keeps Termux loader variables only in `DD_SAVED_*`, restoring them when an
Android/Termux binary is executed. This includes PREFIX and every exported TERMUX_* /
TERMUX__* variable, carried through glibc descendants even with a custom exec environment.
apt, sudo, and unsupported servers continue to need proot.

## Plan

1. Quick win: a native prefix with patched copies of the tools Claude Code and the shell use
   all the time (git, grep, rg, find, du, tar, python3), behind wrappers in the Debian session;
   proot stays the fallback.
2. Brave and Electron apps natively through the launcher (Brave sync, Claude desktop).
3. gcc/node/npm natively (the path hook, `--sysroot`).
4. Re-patching after Debian upgrades (a dpkg hook) and a self-test at start; anything that
   fails falls back to proot.

## Regression checks

`tools/glibc-native/test.sh` runs the container suite with gcc `-Wall -Wextra -Werror`
and patchelf. It covers component symlinks and executable dispatch, no-follow operations,
loader library mirrors, locale selection, original app filenames and adjacent ICU data,
and restored Termux variables in bionic-marker children. Run `bash -n` on the launcher and
`test-container.sh`, then `build.sh` and `test.sh`. Android seccomp, actual locale generation,
Bun Claude and Chromium windows still require device checks.

## Night run 2026-10-29/30: native first, proot as the fallback (step 2)

Goal (Matěj): run as many Debian programs as possible natively; proot only where native
cannot work or fails to start.

Almost every Debian program is started through one launcher, `debian-run` (menu entries from
droiddesk-menu-sync, electron-run, debian-tmux and so claude-tmux/codex-tmux, the USB tools).
`claude-debian` and `codex-debian` call proot-distro directly. So:

A. pathhook: map AF_UNIX socket paths in connect/bind/sendto (`/tmp/.X11-unix`, the D-Bus
   session socket `/tmp/dbus-session`, `/tmp/.virgl_test` for virgl, tmux sockets), so X,
   D-Bus, GPU and tmux work natively without per-program variables.
B. `droiddesk-glibc-native run`: pass through what a desktop program needs (DISPLAY,
   DBUS_SESSION_BUS_ADDRESS, the GPU variables the Debian GUI profile sets, LANG/LC_*,
   TERM, COLORTERM, user variables listed in DD_NATIVE_PASS).
C. `debian-run` native first: when native glibc is ready and not switched off
   (`droiddesk-glibc-native off`, DD_NATIVE=0), and the command is not one that needs proot
   (apt, apt-get, dpkg, sudo, su, passwd, useradd, chroot, mount, systemctl, service,
   proot-distro maintenance), run it natively; otherwise, or when the native preflight fails
   (missing setup, command not found in the rootfs), proot. Every decision goes to
   `$TMPDIR/droiddesk-native.log`. claude-debian and codex-debian the same way.
D. Setup automatically: after Debian is ready the app runs `droiddesk-glibc-native setup`
   in the background (0.2 s when current) unless switched off.
E. Tests per app class on the Tab S10 FE: Claude Code (claude-tmux), Codex, git/node/python,
   an Electron app, Brave from the menu, GTK apps from Debian, GPU (virgl) programs, the USB
   tools; native vs proot, what falls back.

### Step 2 implementation (2026-10-29)

A–D are implemented; E remains device validation. No APK or shipped `pathhook.so` is built
by this change: rebuild the hook with `tools/glibc-native/build.sh` before the APK build.

- `connect`, `bind`, `sendto` and `sendmsg.msg_name` use the file-path mapping for AF_UNIX
  pathname addresses. Abstract/unnamed sockets stay unchanged. Input lengths are respected,
  including a pathname without a terminating NUL; expansion that exceeds `sun_path` falls
  back to the original address. Returned peer/socket names are not translated.
- `run` keeps `DISPLAY=:N` as supplied (default `:0`): Xlib can fall back from its abstract
  socket attempt to `/tmp/.X11-unix/XN`, which the hook maps to `DD_TMP`. DBus defaults to
  `unix:path=/tmp/dbus-session`; an exported address wins. LANG and exported LC_* use the
  locale fallback, TERM/COLORTERM and exported Mesa/GL/Vulkan variables are carried through.
  Space-separated names in `DD_NATIVE_PASS` add user variables. Loader variables, PREFIX,
  TERMUX_*, native runtime controls and shell startup injection variables cannot override
  the clean environment; values pointing into the Termux prefix are excluded. Bionic
  variables remain in `DD_SAVED_*` for Android/Termux children.
- Before dispatch, native bash sources the existing Debian `droiddesk-gui.sh`. This selects
  the same virpipe/zink/llvmpipe policy and GPU off switch as Debian login shells, replacing
  any inherited Termux driver selection. Debian GPU library paths become physical rootfs
  entries in `DD_LIBPATH`; `LD_PRELOAD` and `LD_LIBRARY_PATH` are removed before dispatch.
- `try CMD ...` returns 3 before running CMD for disabled/unprepared native mode,
  a proot-only command, or an unresolved/non-executable command. Bare names
  use `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`; symlinks resolve inside
  Debian, and relative command paths start in Debian root home. Commands outside the rootfs
  are declined. For `bash -c/-lc`, only the first simple command word is checked (also after
  `exec`, `command`, `env` or assignments); this is not a shell parser. Compound scripts can
  still contain operations requiring proot: use `DD_NATIVE=0` for those.
- `debian-run`, `claude-debian` and `codex-debian` call `try` first and retain the existing
  proot invocation on an explicit fallback request or a missing launcher. Native commands get physical Debian
  root HOME/cwd and `bash -lc` login semantics (`/etc/profile` and the usual user login
  profile), including `~/.local/bin`. Claude/Codex keep their existing project startup bodies;
  Claude keeps its private messaging socket logic, harmless with the real app uid too.
  Native sessions put that directory in physical TMPDIR instead of the much longer Debian
  HOME path, which can exceed the Unix socket pathname limit. Proot keeps its cache path.
  Native tmux skips passwd creation and `server-access` when real uid equals the app uid.
  Proot still ensures an entry, checking `grep -q ":x:$uid:" /etc/passwd` before appending
  even when getent misses it, and reads the existing name for `server-access`.
  Existing proot tmux servers use a different uid socket directory; switching mode
  requires starting/attaching sessions in the selected mode.
- Native/proot decisions append reason and command basename only to
  `$TMPDIR/droiddesk-native.log` (about 64 KiB, retaining the last 32 KiB on rotation).
  Each caller creates a private, empty `DD_NATIVE_RESULT` file. Only a preflight decline or
  a supervised quick GUI failure writes `proot` there. Callers remove the file and run
  their existing proot branch once; otherwise they propagate the exact native exit status.
  A program returning 3 no longer causes a terminal command to be repeated. Standalone
  `try` still returns 3 for a declined preflight; standalone GUI quick failures restart
  through `DD_NATIVE=0 debian-run` when no result-file caller is present.
- Once Debian is ready at session start, setup runs on a daemon thread, with output in
  `$TMPDIR/droiddesk-native-setup.log`. Repairs trigger it after successful completion.
  It never waits on the desktop startup path and respects the persistent off marker.

Native routing defaults to on after successful setup (no off marker). Switch it with:

```sh
droiddesk-glibc-native off     # Creates $PREFIX/opt/glibc-native/off; also skips background setup
droiddesk-glibc-native status
droiddesk-glibc-native on      # Removes the marker; setup is still required if not yet prepared
DD_NATIVE=0 debian-run apt-get update  # Force proot for one invocation
```

Explicit `run`, `claude`, patching and self-tests remain available for native diagnostics.
`run_native` exports `DD_NATIVE_ACTIVE=1` into its clean environment and the hook preserves
it across child exec/spawn. If debian-tmux times out waiting for a session on that path,
it records the failure in its log, calls `droiddesk-glibc-native learn tmux` and retries the
whole launcher once with `DD_NATIVE=0`. A private result file carries this signal back to
the Termux caller; a normal command exit status alone cannot trigger it. Future debian-tmux
starts consult the same learned list before launching either host or client. Failed proot
starts and failures attaching to an already started session do not trigger another retry.
`droiddesk-glibc-native reset tmux` allows another native attempt.
The container suite now covers pathname stream/datagram sockets, abstract sockets, overlong
mapping fallback, desktop environment filtering, Debian GPU profile application, on/off and
preflight decisions, command symlinks, login HOME/cwd/profile and bounded argument-free logs.
Android seccomp, X/DBus/virgl connections, real-uid tmux, Bun and Electron self-reexec still
need the device matrix in E; preflight cannot establish support for every program syscall.

### Native launch overhead

On Tab S10 FE, `debian-run true` took 0.53-0.55 seconds natively versus 0.51-0.53 seconds
under proot, although the native program itself takes milliseconds. The launcher target
is below 100 ms; the changes below require a new device measurement before claiming it.

- `try` and `run` no longer need Python for command resolution. Bash walks path components,
  preserving relative links and rooting absolute Debian links in the rootfs, with a limit
  of 40 followed links. Only actual symlinks invoke `readlink`. Internal helpers return
  values in `REPLY`, avoiding command-substitution forks for PATH candidates, locale
  selection and program classification. Classification reuses the preflight target.
- Readiness checks use file tests, including `N/stamp`; hashing remains confined to explicit
  `setup`. Library refresh checks directory mtimes and runs only after changes. Refresh
  also uses the Bash resolver, with private staging files and atomic link publication.
- Exported desktop and Termux variables are enumerated with Bash prefix/attribute
  expansions. No `compgen` subprocesses or pipelines are needed. Native decisions read
  the learned list and bounded log with builtins; external log commands run only on
  directory creation or rotation. Generated callers read fallback files with `$(< file)`.
- Timezone lookup uses the hourly file cache above. Existing login/profile and GUI
  initialization behavior is preserved, so their own commands still contribute to latency.

The container suite prints the average of 20 warm `try /usr/bin/true` calls without a
pass/fail threshold. This measures the sourced `try`/native launch path, excluding the
outer `debian-run` script startup and its result-file creation/removal. It also checks
that successful launches and library refresh do not invoke Python, plus component-link
resolution, the link limit, timezone expiry, cache reuse and explicit TZ handling.
For end-to-end device profiling, use `PS4='+${EPOCHREALTIME} ' bash -x PATH/TO/debian-run true`
and trace `droiddesk-glibc-native` separately with the same PS4; Bash tracing is not
automatically inherited through an executable script. Compare warm runs separately from
the first timezone/library refresh.

### Nové aplikace

Po instalaci balíčku apt automaticky spustí `droiddesk-menu-sync`. Ten vytvoří položky
`Exec=env DD_NATIVE_GUI=1 …/debian-run …`; Electron aplikace předá přes `electron-run`.
Nové aplikace tak používají nativní běh bez ručního `patch`, `app` nebo změny launcheru.
Před startem se automaticky obnoví odkazy nových Debian knihoven. Verze Debian setupu 15
obnoví také existující položky menu při příštím spuštění relace.

Electron se spouští z původního adresáře v rootfs. Hook vrací skutečný program pro
`/proc/self/exe` a zprostředkuje jeho reexec; sousední ICU/resources soubory zůstanou dostupné.
Ruční mirror není potřeba. Nativní `electron-run` přidává `--disable-dev-shm-usage`.

Pokud označený GUI start bez TTY na stdin **i** stdout skončí chybou nebo signálem před
uplynutím přibližně pěti sekund, zapíše se vyřešený název programu do
`$PREFIX/opt/glibc-native/proot-list`. Stejný start se jednou zopakuje pod prootem a příště
jde tento program rovnou do prootu. `env`, `gpu-run` a `electron-run` se při určení názvu
rozbalí; seznam tak nevyřadí všechny aplikace najednou. Neurčitelné GUI shellové wrappery
jdou konzervativně rovnou do prootu. Terminálové příkazy se po chybě neopakují ani neučí.

```sh
droiddesk-glibc-native status          # Vypíše i naučené programy
droiddesk-glibc-native learn tmux      # Vynutí proot pro další starty tmux relací
droiddesk-glibc-native reset obsidian  # Znovu dovolí nativní pokus tohoto programu
droiddesk-glibc-native reset --all     # Vymaže celý naučený seznam
```

Úspěšné ukončení ani chyba po pěti sekundách seznam nemění. Detekce sleduje návratový kód
spouštěného procesu; neodhalí pád potomka, pokud launcher už úspěšně skončil. Přímé syscally
mimo hook zůstávají omezením kompatibility a patří do ověření na zařízení. Kontejnerové
testy pokrývají původní Electron ELF bez mirroru, rychlou chybu i signál, pomalou chybu,
ochranu TTY, reset, souběžné zápisy seznamu a jednoznačný restartovací protokol.

The regression suite also covers NSS against a fake passwd/group rootfs (including `_r`
buffer limits, enumeration, thread-local results and getgrouplist capacity/deduplication),
the explicit learn command and all eight new libc syscall numbers using synthetic arm64
ELFs. Real Android seccomp and native/proot tmux startup still require device validation.
