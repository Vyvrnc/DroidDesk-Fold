# Debian (glibc) programs without proot

Status: spike done on the Galaxy Tab S10 FE (night 2026-10-29/30, fold.89/90). Full notes on the
tablet in `~/glibc-spike/FINDINGS.md`; this file is the design that came out of it. Step 1 now
has an opt-in Termux-side launcher; device validation of this implementation is still required.

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
   rest goes through a small glibc path hook (`pathhook.so`, about 60 lines, loaded with
   `ld.so --preload`): open/stat/access/opendir/readlink/realpath/dlopen … of `/usr /etc /var
   /opt /lib /bin /sbin` → the rootfs, `/tmp`, `/dev/shm` → Termux's tmp. node, npm, fc-match and
   perl XS run with it.
4. **Launcher with a clean environment.** Termux variables (GIO_MODULE_DIR …) made glibc
   programs load bionic modules ("invalid ELF header"). Brave also needs
   `--disable-dev-shm-usage` with TMPDIR and `DISPLAY` set to the full path of the X socket.

## What stays under proot

Blocked calls that matter: the set*uid/gid family except setresuid (apt drops to `_apt`, sudo),
`accept` (accept4 works; servers using plain accept), SysV IPC (shm/sem/msg), POSIX mqueues,
keyctl, io_uring, futex2, the mount API. So `apt`, `sudo`, and servers stay on proot; returning
0 instead of ENOSYS for set*id is a possible later step (a design decision: it hides failures).

## How to use (step 1)

Build the shipped arm64 hook with `bash tools/glibc-native/build.sh` before building the app.
The app installs the launcher and its assets at startup; it never runs setup automatically.
From a **Termux-side terminal outside proot**, with Debian already installed:

```sh
droiddesk-glibc-native setup
droiddesk-glibc-native selftest
droiddesk-glibc-native run git --version
droiddesk-glibc-native run /bin/sh -c 'python3 -c "import ssl"; git --version'
droiddesk-glibc-native claude
```

This is opt-in: existing Debian and Claude launchers continue to use proot. On failure, use
those launchers. Native children inherit the path hook, so a tool chain must start through
this launcher from Termux; launching it under proot still traces its descendants. The hook
maps Debian system paths into the rootfs and temporary paths into Termux tmp; HOME and shared
Android paths stay unchanged. `claude` uses the existing Debian root installation and its
configuration (`HOME` is the physical rootfs `root` directory).
`getcwd` and `realpath` remove the rootfs prefix only for mapped system directories, preserving
the physical HOME. Mapping covers interposable libc calls; direct syscalls, static programs,
libc-internal calls and existing absolute symlinks escaping the rootfs are outside this hook.

Run `setup` again after Debian upgrades. It hashes the source loader, libc, optional libuv
and shipped helpers, and rebuilds patched copies only when they change. Unknown instruction
patterns fail with an error before replacing the current copies. Nothing installs packages
automatically: setup needs Termux `python3` (`pkg install python`).

For a binary that re-executes `/proc/self/exe`, install Termux patchelf with `pkg install
patchelf`, then run `droiddesk-glibc-native patch /usr/bin/PROGRAM`. It prints the path of a
copy under `$PREFIX/opt/glibc-native/bin`; launch that path with `run`. RPATH is patched before
the interpreter in separate calls. A DT_NEEDED entry keeps the hook loaded on re-exec without
leaking LD_PRELOAD into Termux tools. Existing `$ORIGIN` entries point back to the source
directory. Re-run `patch` after upgrading that program; `claude` refreshes its native binary
copy on every launch. The npm Claude installation instead runs Debian node with `cli.js`.

DNS uses inherited fd 63; programs that close it need separate validation. The clean native
environment keeps Termux loader variables only in `DD_SAVED_*`, restoring them when an
Android/Termux binary is executed. apt, sudo, and unsupported servers continue to need proot.

## Plan

1. Quick win: a native prefix with patched copies of the tools Claude Code and the shell use
   all the time (git, grep, rg, find, du, tar, python3), behind wrappers in the Debian session;
   proot stays the fallback.
2. Brave and Electron apps natively through the launcher (Brave sync, Claude desktop).
3. gcc/node/npm natively (the path hook, `--sysroot`).
4. Re-patching after Debian upgrades (a dpkg hook) and a self-test at start; anything that
   fails falls back to proot.
