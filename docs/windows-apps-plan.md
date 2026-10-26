# Windows x86/x64 apps — plan (2026-10-17)

Goal: run Windows utilities (device configurators, Modbus/serial tools, small editors) in
DroidDesk's Debian, from the XFCE menu or by opening an `.exe` in Thunar, with a COM port that is
our `/tmp/ttyUSB0`. Two user-selectable backends:

- **A — Box64 + x86_64 Wine (WoW64)**: the whole Wine (Unix side included) is an x86_64 build run
  by Box64; Box64 wraps common libraries (libc, X11, GL, Vulkan, Pulse, freetype) to Debian's arm64
  ones. 32-bit apps run through Wine's new WoW64 inside the same x86_64 process, so no box86/box32.
- **B — aarch64 Wine with ARM64EC + FEX**: Wine itself is native arm64; only the Windows app code
  is emulated by FEX built as a PE DLL (`libarm64ecfex.dll` for x64, `libwow64fex.dll` for x86).
  Same model as Hangover, Proton ARM64 (Valve), GameNative and Winlator's ARM64EC containers.

Primary target is 2D Win32/.NET utilities. Games are secondary: X11 presentation is software
(no DRI3 with Termux:X11), so DXVK/Vulkan output is copied through the CPU.

Marks: **[v]** verified on 2026-10-17 (web URL or this repo), **[u]** unverified / inferred —
to be checked in Phase 0 or at implementation.

## Where it plugs in (this repo, branch fold-dex)
- Debian is **13 trixie arm64** under proot-distro [v] (`tools/mesa-kgsl/NOTES.md`,
  `tools/usb-blk-test/serial.sh`, glibc 2.41). Launchers written by `writeDebianLauncher()` in
  `LinuxRuntime.kt`: `start-debian` (login shell), `debian-run CMD…` (one command; session bus
  is Debian's own `/tmp/dbus-debian` with gnome-keyring while it runs, else the XFCE bus, which does
  not answer Debian clients, so `notify-send` from Debian reaches no notification daemon; `DISPLAY=:0`, binds `/tmp` → Termux tmp and `/storage`), `debian-setup` (idempotent root
  setup, stamps `/etc/droiddesk/setup-version` with `DEBIAN_SETUP_VERSION`, currently 14; rerun in
  the background at session start while the stamp differs).
- **Catalog item** = three places:
  1. `app/lib/screens/apps/app_catalog_screen.dart`: `_FeaturedApp('<id>', name, description,
     icon, color, optional: true)` (cf. `claude_code`, `codex`).
  2. `LinuxRuntime.getOptionalAppsStatus()`: `"<id>" to is…Installed()` — a file check inside
     `debianRootfs(baseDir)` with `NOFOLLOW_LINKS` (Debian's absolute symlinks do not resolve on
     the Android side).
  3. `installOptionalApp()`'s `when (appId)`: `"<id>" -> install…(onProgress)`. Pattern of
     `installCodex`: install minimal Debian when missing, wait while `debianSetupRunning` (apt
     lock), run the install through `debian-run bash -c '…'`, verify with the status check.
- **Pinned artifacts**: `MESA_KGSL_VERSION` + `MESA_KGSL_SHA256` constants, release tag
  `mesa-kgsl-<ver>` on `github.com/Vyvrnc/DroidDesk-Fold`, downloaded by `debian-setup` with
  `sha256sum -c`, unpacked to `/opt/mesa-kgsl` with `.droiddesk-version`, rollback to `.old` on
  failure. Built by `tools/mesa-kgsl/build.sh` in an emulated `--platform linux/arm64`
  `debian:trixie` Docker container. Same pattern for `droiddesk-eth`.
- **Menu**: `droiddesk-menu-sync` (apt `DPkg::Post-Invoke` hook) mirrors
  `/usr/share/applications/*.desktop` and `/usr/local/share/applications/*.desktop` into XFCE as
  `debian-*.desktop` with `Exec=debian-run <exec>`; skips `Terminal=true`, `NoDisplay`,
  `OnlyShowIn`; keeps other keys (`MimeType=` survives). It does **not** read subfolders or
  `~/.local/share/applications` in Debian — Wine's own winemenubuilder entries would be missed.
- **GPU**: `/etc/droiddesk/gpu-env.sh` (sourced by `gpu-run`): Adreno → `/opt/mesa-kgsl`, Zink
  over Turnip/KGSL, `VK_ICD_FILENAMES` = freedreno ICD; non-Adreno → virpipe (virgl over ANGLE)
  with `MESA_GL_VERSION_OVERRIDE=4.3`. Commit `0fda529` (fold.54): wineboot already ran on the
  tablet and hung on explorer.exe with `LIBGL_ALWAYS_SOFTWARE`; fixed by hiding `/dev/dri` [v].
- **Audio**: PulseAudio on the Termux side with `module-native-protocol-tcp listen=127.0.0.1`;
  proot-distro sets `PULSE_SERVER=127.0.0.1` in Debian [v]. winepulse uses it as is.
- **Serial**: `droiddesk-serial` links `/tmp/ttyUSB<n>` to a pty slave; DTR/RTS only through
  `droiddesk-serial lines …` (a pty has no modem lines) [v] (`docs/usb-serial-plan.md`).
- **Locales**: debian-setup generates `en_US`, `cs_CZ` and the device locale; `LANG=C.UTF-8`.
- **Open handler**: `droiddesk-open` (Termux side) uses `mimeapps.list`, else an Android app.

## Research: backend A (Box64 + x86_64 Wine)
- **Box64 releases** [v] https://github.com/ptitSeb/box64/releases: latest **v0.4.4** (2026-08-02),
  v0.4.2 (2026-04-20, "SteamRT3 & new Proton 11"), v0.4.0 (2026-01-03). Assets: source plus
  `box64-bundle-x86-libs-v0.4.4.tar.gz` (+ `.sha256`) and `box64-bundle-x86-pkgs-v0.4.4.tar.gz`;
  **no prebuilt arm64 binary**.
- **Debian packages** [v]: trixie (stable) `box64 0.3.4+dfsg-1` (also `box64-rpi4`,
  `box64-rk3588`) https://packages.debian.org/source/trixie/box64; unstable/testing
  `0.4.3-3+dfsg-1` (2026-06-13 / 06-19) https://tracker.debian.org/pkg/box64-rpi4. Trixie's is a
  year old — fine for the Phase 0 spike only. ryanfortner/box64-debs: current state not checked [u].
- **Build flags** [v] (`CMakeLists.txt`, `docs/COMPILE.md` on main): the CHRoot/PRoot recipe is
  `-D ARM64=1 -DBAD_SIGNAL=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo` (BAD_SIGNAL = "workaround for
  incoherent si_info on SIGSEGV", for Android-mixed kernels). Snapdragon profiles: `SD845`,
  `SD865`, `SD888`, `SD8G2` (armv9-a, nosve), `SDORYON1` (armv8.6-a+crypto+sm4+sha3+fp16),
  `SD8EG5` (armv8.7-a **+sve+sve2**, 8 Elite *Gen 5*). No profile is named for the 8 Elite (SM8750,
  Fold 7); SD8EG5 must not be used there (SVE; SM8750 lacks SVE [u]). Recommendation: generic
  `ARM64=1 BAD_SIGNAL=ON ARM_DYNAREC=ON` — the dynarec probes CPU features at runtime and one
  binary serves both the Fold 7 and the Tab S10 FE (Exynos 1580); try `SDORYON1` only if Phase 5
  shows a gain. Also present: `WOW64` (builds `wowbox64.dll`), `ARM64EC`, `BOX32`, `WINLATOR_GLIBC`.
- **Wine for Box64** [v] https://raw.githubusercontent.com/ptitSeb/box64/main/docs/WINE.md:
  recommends Kron4ek "Wine [WOW64]" builds or Proton-GE; with an x86_64 WoW64 build Box64 alone
  runs 32-bit apps; WineD3D is slow for 32-bit. Kron4ek latest **11.19** (2026-10-03), assets
  `wine-11.19-amd64-wow64.tar.xz`, `-staging-amd64-wow64`, `-staging-tkg-amd64-wow64`
  https://github.com/Kron4ek/Wine-Builds/releases. WineHQ amd64 debs would need an amd64 multiarch
  dpkg in an arm64 rootfs — rejected (fragile, pulls x86_64 libs everywhere).
- **Wine 11.0** (stable, 2026-01) [v]: new WoW64 "fully supported", `wine64` loader removed — one
  `wine` picks the mode https://www.linux.org/threads/linuxiac-wine-11-0-brings-fully-supported-wow64-mode-and-linux-kernel-ntsync-support.61207/latest
- **Env vars** [v] (`docs/USAGE.md`): `BOX64_DYNAREC_BIGBLOCK` (3 = tuned for Wine programs, 0 for
  JIT/thread-heavy apps, default 2), `BOX64_DYNAREC_STRONGMEM` (default 0; raise on multithread
  crashes), `BOX64_DYNAREC_SAFEFLAGS` (default 1, 2 safest), `BOX64_DYNAREC_CALLRET` (default 2),
  `BOX64_MMAP32` (default 1, 32-bit-compatible maps for WoW64), `BOX64_DYNACACHE` (on-disk dynarec
  cache, default 1), `BOX64_AVX` (default 2 on arm64), `BOX64_LOG`, `BOX64_NOBANNER`,
  `BOX64_RCFILE`. Per-exe tuning goes into an rc file (`[app.exe]` sections), not the wrapper.
- **proot**: Box64Droid warns that box64 in proot "can lead to some issues and instabilities"
  [v, anecdotal] https://github.com/Ilya114/Box64Droid. Concrete risks: SIGSEGV delivery under
  ptrace (hence BAD_SIGNAL); every syscall of the x86_64 Wine (wineserver round-trips) pays both
  Box64's wrapping and proot's ptrace cost.

## Research: backend B (aarch64 Wine ARM64EC + FEX)
- **Mechanism** [v]: Wine 10.0 added the x86_64 emulation interface for ARM64EC; Wine ships no
  emulator; an external DLL named in `HKLM\Software\Microsoft\Wow64\amd64` is used; FEX built as
  ARM64EC implements it (Wine 10.0 release notes). For x86 guests upstream `dlls/wow64/syscall.c`
  reads the default value of `HKLM\Software\Microsoft\Wow64\x86`, default `xtajit.dll` on an arm64
  host, **no environment variable** [v]
  https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/wow64/syscall.c. Julliard (WineHQ
  bug 58101): copy `libwow64fex.dll` to system32 and name it in that key; Wine must be built with
  i386 (`--enable-archs=aarch64,i386`) [v].
- **Upstream Wine vs forks** [v] https://wiki.fex-emu.com/index.php/Development:ARM64EC: upstream
  "can be used" but lacks pieces; bylaws' fork `github.com/bylaws/wine/tree/upstream-arm64ec`
  gives "the best results"; build `--enable-archs=arm64ec,aarch64,i386 --with-mingw=clang` with
  the ARM64EC toolchain `github.com/bylaws/llvm-mingw`. FEX is built twice
  (`-DMINGW_TRIPLE=arm64ec-w64-mingw32` and `aarch64-w64-mingw32`, libdir
  `/usr/lib/wine/aarch64-windows`).
- **FEX** [v] https://github.com/FEX-Emu/FEX/releases: latest **FEX-2610** (2026-10-07: "Don't
  hardcode NT syscall numbers for ARM64EC", WOW64 thread fix), 2609, 2608 ("Support mixed
  WoW64/ARM64EC processing"). DLLs prebuilt in the PPA package `fex-emu-wine` **2610-1**
  (noble/jammy/resolute) https://launchpad.net/~fex-emu/+archive/ubuntu/fex/+packages — PE DLLs do
  not depend on the host libc, so the .deb can be unpacked on Debian. FEX is MIT.
- **Hangover = ready-made backend B for Debian 13** [v] https://github.com/AndreRH/hangover:
  aarch64 Wine running x86/x86_64 apps; x86_64 → `libarm64ecfex.dll` (default) or `xtajit64.dll`;
  x86 → `wowbox64.dll` (**default**) or `libwow64fex.dll`; selection by env **`HODLL64`** /
  **`HODLL`** (Hangover's own patch, not upstream). Debian 11/12/13 and Termux. Latest
  **hangover-11.16** (2026-08-30; FEX 2608, Box64 v0.4.4): `hangover_11.16_debian13_trixie_arm64.tar`
  (280 MB), `hangover_11.16_dlls.tar` (28.7 MB) https://github.com/AndreRH/hangover/releases.
  Known in 11.16: automatic wine-mono download broken, workaround `WINEDEBUG=wininet wine wineboot -u`.
  Exact .deb names inside the tar and installed size [u].
- **Not usable here** [v]: GameNative `proton-wine` 11.0-2 (`proton-wine-11.0-2-arm64ec.wcp.xz`,
  near-daily, "testing purposes only") and Winlator `.wcp` builds target **Android bionic** (SDK 28,
  16 KB page support) https://github.com/GameNative/proton-wine/releases — they need Termux-native
  Wine, not our glibc Debian. Valve's Proton 11 ARM64 is a beta inside Steam, no standalone
  download [v, news only]. Fedora COPR `lacamar/wine-arm64ec`: RPMs, its author says "everybody
  else should avoid this repo" [v].
- **DXVK/VKD3D**: x64 DXVK DLLs work under ARM64EC but their code is emulated; ARM64EC builds of
  dxvk/vkd3d-proton need a meson cross file for `arm64ec-w64-mingw32` (FEX wiki) [v]. A prebuilt
  ARM64EC DXVK for glibc Linux: not found [u].
- **Kernel/proot needs**: FEX here is a pure userspace JIT inside a Windows process — no Linux
  FEX, binfmt, x86 rootfs or FEXServer. It needs mmap/mprotect and SIGSEGV/SIGILL handling, which
  aarch64 Wine needs anyway [u, inferred]. No report found of Hangover in proot-distro on Android
  [u]; Hangover ships Termux packages, so the stack runs on Android kernels [v].

## Phase 0 results (Fold 7, 2026-10-20)
- Page size 4096, no SVE/SVE2 (Oryon SM8750, 8 cores), Box64 reports 39-bit VA.
- **B (Hangover 11.16) works**: `wineboot -i` 82 s, prefix 2.6 GB; notepad x86 via FEX 6.9 s
  (start dominated by wineserver), RSS ~170 MB; PuTTY w32/w64 OK. 7-Zip MIPS: native arm64 17035,
  x64 arm64ecfex 9159 (54 %), x86 wow64fex 8105 (48 %), x86 wowbox64 7884 (46 %). Hangover writes
  `xtajit*.dll` into the Wow64 registry keys; `HODLL`/`HODLL64` work.
- **A (Debian box64 0.3.4 + Kron4ek 11.16 wow64) fails**: `could not load kernel32.dll, c0000135`.
  Box64 bypasses the preloader itself (no binfmt needed), no signal crashes under proot. Suspect old
  Box64 + 39-bit VA → retry with Box64 0.4.4 before dropping A.
- `wine notepad` runs the native arm64 notepad; test emulation with `syswow64\notepad.exe` or x86 exes.
- Not tested yet: Czech keyboard input by hand, serial COM.
- **Phase 0b, A with own Box64 v0.4.4** (`2f130fa`, `-DARM64=1 -DBAD_SIGNAL=ON -DARM_DYNAREC=ON`,
  built on the phone): x64 works, the c0000135 was old Box64 0.3.4. `wineboot -i` 306 s, prefix
  335 MB; notepad x64 18.8 s, PuTTY w64 35.0 s; RSS of Wine processes ~1.0–1.1 GB; 7-Zip x64
  9539 MIPS (56 %, about the same as FEX). **32-bit x86 fails** (`syswow64\notepad.exe` cannot be
  opened, PuTTY w32 c0000135): WoW64 Wine needs Box64 built with `-DBOX32=ON` (not tried yet).
- **Phase 0c, Box64 v0.4.4 with `-DBOX32=ON`**: the 32-bit path works. New prefix: `wineboot -i`
  307 s, 629 MB; notepad x86 19.0 s, PuTTY w32 24.4 s, w64 21.0 s; RSS ~1.5–1.6 GB. 7-Zip
  x64 15367 / x86 12363 MIPS (90 % / 73 % of native) on an idle phone, not comparable with the
  earlier runs (other load); to be repeated back to back with all variants. ⚠️ `make install`
  outside DESTDIR overwrites `/etc/box64.box64rc` (conffile of Debian's box64 package) and adds
  `/etc/binfmt.d/*` and `/usr/lib/box64-i386-linux-gnu`; `tools/box64/build.sh` stages instead
  and now builds with BOX32.
- Conclusion after 0b: B is the default for 2D utilities (2–5× faster start, ~6× less memory, x86
  and x64 out of the box). A stays only as an x64 fallback for apps that break under FEX, and only
  if BOX32 makes the 32-bit path work. Serial COM still untested (no adapter at hand).

## Page size gate (both backends)
Wine assumes 4 KB pages: Asahi runs x86 Wine/FEX in a 4 KB muvm VM on its 16 KB host; WineHQ bug
52715 (crash on Apple M1, 16K suspected) [v, indirect]
https://list.winehq.org/hyperkitty/list/wine-bugs@list.winehq.org/thread/OIK2DBX7Y2DUNXLBWYU5TPO3M4CGOYXE/.
Android 15+ supports 16 KB kernels, opt-in [v] https://source.android.com/docs/core/architecture/16kb-page-size/16kb.
Fold 7 (Android 16) and Tab S10 FE page size **[u]** — first command of Phase 0 is
`getconf PAGESIZE` in Debian. 16384 = stop; only a VM would help.

## Common design
- **Install root** `/opt/droiddesk-wine/` in Debian:
  - `box64/` — our Box64 build (`bin/box64`, `etc/box64.box64rc`); `wine-x64/` — Kron4ek
    `amd64-wow64` unpacked; each with `.droiddesk-version`.
  - Backend B from the Hangover debs, `apt-get install ./hangover-*.deb` (owns `/usr/bin/wine`,
    `/usr/lib/wine`); Debian's own `wine` must not be installed alongside — check and refuse. If the
    debs conflict: own `wine-arm64ec` tarball (see Artifacts).
- **Wrapper** `/usr/local/bin/droiddesk-wine` (POSIX sh, written by the install step, not by
  debian-setup — most users never install Wine):
  ```
  droiddesk-wine [--backend box64|fex] [--prefix DIR] [run] APP.exe|APP.lnk [args]
  droiddesk-wine cfg | explorer | taskmgr | regedit | kill | backend [box64|fex]
  droiddesk-wine com [N] [/tmp/ttyUSB0]    # (re)link COMn, default COM3 -> /tmp/ttyUSB0
  droiddesk-wine menu-sync | doctor | uninstall [--backend X] [--purge-prefix]
  ```
  - Default backend in `/etc/droiddesk/wine.conf` (`BACKEND=box64`), user override
    `~/.config/droiddesk-wine/backend`, per-app override `~/.config/droiddesk-wine/apps`
    (`modpoll.exe=fex`). Switching = writing that file; the next start uses it.
  - **Separate prefixes** `~/.wine-box64` and `~/.wine-fex`: different Wine builds; a wineserver
    of one must never touch the other's prefix; prefix downgrades are unsupported. Shared user data
    instead: `D:` → `/storage/emulated/0`, prefix `Documents` symlinked to
    `/storage/emulated/0/Documents`, `Z:` stays `/`.
  - Sets `WINEPREFIX`, `WINEDEBUG=-all` (unless `DROIDDESK_WINE_DEBUG`), `LANG=cs_CZ.UTF-8` when
    the Android locale is Czech (ANSI codepage 1250 follows it; else `en_US.UTF-8`),
    `WINEDLLOVERRIDES=winemenubuilder.exe=d` (we write menu entries ourselves), sources
    `/etc/droiddesk/gpu-env.sh`. Backend A: `exec box64 /opt/droiddesk-wine/wine-x64/bin/wine …`
    — there is no binfmt in proot, so Box64 must catch Wine re-executing itself, wineserver and
    wine-preloader [u: Box64 handles these by name; verify in Phase 0]. Backend B:
    `HODLL64=libarm64ecfex.dll`, `HODLL=wowbox64.dll`; `--x86 fex` sets `HODLL=libwow64fex.dll`.
  - First run per prefix: `wineboot -i` with a `notify-send` progress note, then wine-mono from
    our pinned asset (`msiexec /i wine-mono-*.msi`) — .NET Framework 4.x is common among device
    configurators. No wine-gecko (rarely needed).
- **X11**: `DISPLAY=:0` from debian-run; winex11 over Termux:X11. Virtual desktop off by default;
  available in `droiddesk-wine cfg` for tools whose windows misbehave with xfwm4/DeX.
- **Fonts**: Wine's builtin fonts replace Tahoma/MS Sans Serif; add `fonts-liberation2` and
  `fonts-dejavu-core` (Czech diacritics); `/system/fonts` is already in fontconfig. No corefonts
  by default (licence click-through).
- **COM ports**: Wine maps `COMn` to `$WINEPREFIX/dosdevices/comN` symlinks and autodetects only
  `/dev` serial devices [u: exact list], never `/tmp`. The wrapper links `com3 -> /tmp/ttyUSB0`,
  `com4 -> /tmp/ttyUSB1` (COM1/2 left for tools that hardcode them: `droiddesk-wine com 1`).
  Before starting an app it checks the target exists, else tells the user to open the port in the
  USB window. Wine's serial code uses termios (fine on a pty) and `TIOCMGET/TIOCMSET` for
  DTR/RTS/CTS, which fail on a pty [u: how Wine reports that — tools calling
  `EscapeCommFunction(SETDTR)` may show an error].
- **Audio**: nothing to do (winepulse → `PULSE_SERVER=127.0.0.1`); in A, Box64 wraps libpulse [u].
- **GPU**: default **wined3d over GL** (Zink/Turnip on Adreno, virpipe elsewhere) — enough for
  GDI and old D3D9 configurators. Vulkan: winevulkan → Turnip via `VK_ICD_FILENAMES`. DXVK
  optional per prefix (`droiddesk-wine dxvk on|off`), Adreno only; expect low FPS (software present).
- **Menu and files**:
  - Our entries in Debian `/usr/local/share/applications/`: `droiddesk-wine.desktop` ("Spustit ve
    Windows (Wine)", `MimeType=application/x-ms-dos-executable;application/x-msdownload;application/x-msi;application/x-ms-shortcut;`,
    `Exec=droiddesk-wine %f`), `droiddesk-winecfg.desktop` ("Nastavení Windows (Wine)"),
    `droiddesk-wine-explorer.desktop` ("Soubory Windows"). droiddesk-menu-sync mirrors them.
  - `droiddesk-wine menu-sync` (runs after every `droiddesk-wine` process tree exits): find `.lnk`
    files under both Start Menu folders of the prefix, write one
    `/usr/local/share/applications/wine-<backend>-<name>.desktop` per shortcut with
    `Exec=droiddesk-wine --backend <b> "<unix path of .lnk>"`, remove entries whose `.lnk` is gone,
    then call droiddesk-menu-sync. Icons from the .exe via `icoutils` (`wrestool`/`icotool`),
    generic icon as fallback.
  - Thunar double-click: `mimeapps.list` default for those MIME types →
    `debian-droiddesk-wine.desktop`. **Path issue**: Thunar passes Termux-side paths; only
    `/storage/…` and `/tmp/…` are the same in Debian. The wrapper refuses other paths with a hint
    ("copy the .exe to Downloads") [u: or bind the Termux home into Debian — decide in Phase 3].
- **Terminal profile**: none needed; `droiddesk-wine` works from any Debian terminal.

## Catalog integration
- One catalog item **`windows_apps` "Windows aplikace"** with a backend choice, not two items: the
  wrapper, menu and MIME defaults are shared, two items would fight over `mimeapps.list`. Install
  dialog: "Box64 (doporučeno)" / "FEX (ARM64EC)" / "Obojí"; switch later in the item's detail
  ("Výchozí backend") → `debian-run droiddesk-wine backend fex`.
- Kotlin: `"windows_apps" to isWindowsAppsInstalled()` (`usr/local/bin/droiddesk-wine` and
  `etc/droiddesk/wine.conf` in the rootfs, NOFOLLOW_LINKS); `installWindowsApps(backends,
  onProgress)` modelled on `installCodex` (Debian first, wait for `debianSetupRunning`, then
  `debian-run bash /usr/local/share/droiddesk/wine-install.sh box64 fex`). The install script is an
  APK asset (like `droiddesk-usb.py`) copied into the rootfs, so it updates with the APK without
  bumping `DEBIAN_SETUP_VERSION`.
- Constants next to Mesa's: `BOX64_VERSION`/`_SHA256`, `WINE_X64_VERSION`/`_SHA256`,
  `HANGOVER_VERSION`/`_SHA256`, `WINE_MONO_VERSION`/`_SHA256`, passed to the script as arguments.
  Script = Mesa's pattern: download to `/tmp`, `sha256sum -c`, unpack to
  `/opt/droiddesk-wine/<x>.new`, swap, keep `.old` until success, write `.droiddesk-version`.
- Updates: debian-setup gains "if `/etc/droiddesk/wine.conf` exists and a pinned version differs,
  rerun wine-install.sh for the installed backends" (bump `DEBIAN_SETUP_VERSION` once; 11 is taken by xclip, fold.65, and 12 by libxcb-keysyms1 and
  the GPU self-test, fold.69, 13 by gpu-run Vulkan-only, fold.70, and 14 by gnome-keyring, electron-run and URL schemes,
  fold.72, so Windows apps use 15).
  Wine updates the prefix itself on the next start after a version change.
- Uninstall (catalog "Odinstalovat"): `droiddesk-wine uninstall` removes `/opt/droiddesk-wine`,
  purges `hangover-*`, our .desktop files and MIME defaults; **prefixes are kept** (user data,
  licences) and named in the message; `--purge-prefix` moves them to
  `~/.wine-<b>.removed-<date>` (archive, not delete).

## Artifacts (pinned, sha256, our GitHub releases)
| Asset (tag) | Source | Size [u] |
|---|---|---|
| `box64-0.4.4-1-arm64.tar.xz` (`box64-0.4.4-1`) | `tools/box64/build.sh`: tag v0.4.4 in emulated `debian:trixie` arm64 Docker like Mesa, `-DARM64=1 -DBAD_SIGNAL=ON -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo` | ~5–10 MB |
| `wine-x64-11.16-1.tar.xz` | mirror of Kron4ek `wine-11.16-amd64-wow64.tar.xz` (same Wine version as Hangover for a fair A/B; move both together) | ~70 MB dl, ~450 MB disk |
| `hangover-11.16-1-trixie-arm64.tar` | mirror of `hangover_11.16_debian13_trixie_arm64.tar` | 280 MB dl, ~1 GB disk |
| `wine-mono-<ver>-x86.msi` | mirror of the wine-mono release matching the Wine version | ~80 MB dl, ~250 MB disk |
| `box64-bundle-x86-libs-v0.4.4.tar.gz` (optional) | upstream release | only if a wrapped lib is missing |

- Mirror rather than link upstream: assets can be deleted or retagged; the mirror keeps the
  sha256 pin meaningful. LGPL (Wine, Hangover): attach the source tarball of the same tag to the
  same release. Box64 and FEX are MIT (keep their licence files in the tarballs).
- Fallback for B if Hangover's debs conflict or lag: `tools/wine-arm64ec/build.sh` (bylaws
  llvm-mingw; Wine `--enable-archs=arm64ec,aarch64,i386`; FEX-2610 built twice; Box64
  `-DWOW64=ON` for `wowbox64.dll`) — a multi-hour emulated build like Mesa's. Not in v1.
- Disk (estimate [u]): A ≈ 0.8 GB, B ≈ 1.3 GB, both plus two prefixes ≈ 2.5 GB. The install
  dialog shows the estimate and checks free space first.

## Phases and effort
| Phase | Content | Effort |
|---|---|---|
| 0 Spike (by hand, Fold 7 + tablet) | `getconf PAGESIZE`; A: `apt install box64` (0.3.4) + Kron4ek tarball, `box64 wine winecfg`, notepad; B: Hangover trixie debs, `wine notepad`, `HODLL64`/`HODLL`; note what breaks under proot | 1–1.5 d |
| 1 Artifacts | `tools/box64/build.sh`, mirrored releases, sha256 constants | 1 d (+ build wait) |
| 2 Wrapper + install script | `droiddesk-wine`, `wine-install.sh`, prefixes, wine-mono, COM links, gpu-env | 2 d |
| 3 App integration | catalog item, Kotlin status/install, debian-setup update block, .desktop + MIME, Start Menu sync, uninstall | 1.5 d |
| 4 Serial | COM tests against the harness and a real adapter, DTR/RTS behaviour | 1 d |
| 5 GPU/perf | wined3d vs DXVK on Adreno, virpipe on the tablet, A/B timing table | 1 d |
| Total | | **~7.5–8 d** |

## Test plan (device)
Record per backend and device: works / slow / broken, cold and warm start time, RSS.
1. `droiddesk-wine doctor`: page size, backend files and versions, prefix, DISPLAY, Pulse,
   `/tmp/ttyUSB*`.
2. `droiddesk-wine cfg`: window draws, Czech text, Audio tab test sound, Drives tab shows D:.
3. `notepad`: type ěščřžýáíéůú, save to `D:\Documents`, open the file in Mousepad.
4. Same app 32- and 64-bit: **PuTTY** official `w32` and `w64` builds
   (https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html) — serial session on COM3,
   exercises both WoW64 paths; **HxD** and **Notepad++** (32/64 installers) for GUI, installer,
   uninstaller and Start Menu sync.
5. Modbus/serial tools: **modpoll** Windows console build (proconX, free) —
   `modpoll -m rtu -b 9600 -p even -a 1 -r 1 -c 5 COM3`; **QModMaster** (Qt, SourceForge);
   **Modbus Poll** (Witte Software, 30-day trial, MFC GUI); **Simply Modbus Master** (trial)
   [u: current licences and bitness — check at download]. Plus one real vendor configurator the
   team uses (ideally .NET, for wine-mono).
6. Serial against the harness: extend `tools/usb-blk-test/serial.sh` (Docker `debian:trixie` on the
   PC, x86_64) with a step that installs native x86_64 Wine and runs `modpoll.exe` on
   `COM3 -> /tmp/session/ttyUSB0` against `fake_serial_bridge.py`'s Modbus RTU slave: proves the COM
   mapping, baud/parity propagation (`P 9600 8 1 2` must reach the bridge) and what DTR/RTS calls do
   — without an emulator in the loop. On device: same with a USB-RS485 adapter and a real meter.
7. GPU: **GPU Caps Viewer** (Geeks3D, free; GL and Vulkan demos) — renderer must read zink/Turnip
   (Adreno) or virgl (tablet); one free D3D9 demo with wined3d, then with DXVK on Adreno; note FPS.
8. A vs B: `wineboot -i` time, cold/warm start of notepad and Modbus Poll, **7-Zip** `7z.exe b`
   (32- and 64-bit) for a dynarec comparison independent of graphics; B's x86 path both with
   `HODLL=wowbox64.dll` and `HODLL=libwow64fex.dll`.

## Risks and open questions
1. **16 KB pages** on either device → plan void (first check of Phase 0).
2. **proot**: signal delivery/SMC detection under ptrace, syscall overhead on wineserver traffic;
   Box64-in-proot reported unstable by others. B runs Wine natively and emulates only app code —
   likely more robust under proot [u].
3. **Hangover packaging**: debs may pull many deps or conflict with Debian's `wine`/`libwine`;
   installed size unknown. Fallback: own arm64ec build (multi-day).
4. **Serial modem lines**: tools that toggle DTR/RTS or wait for CTS get errors on a pty. Later
   option: an `LD_PRELOAD` shim mapping `TIOCM*` on our ptys to `droiddesk-serial lines` — simple
   for B (native aarch64 Wine), for A it would have to be an x86_64 shim under Box64 [u].
5. **Paths from Thunar** (Termux side vs Debian side), see Menu and files.
6. **Android process kills** (phantom process killer) on long sessions — Wine adds wineserver and
   several processes to the desktop's count.
7. **.NET tools**: wine-mono covers many; some need real .NET (`winetricks dotnet48`) — slow and
   fragile under emulation; out of scope for v1.
8. **Kron4ek x86_64 Wine's unwrapped dependencies** (e.g. gnutls, libunwind) may need
   `box64-bundle-x86-libs` [u].
9. **Upstream churn**: FEX monthly, Hangover and Box64 roughly quarterly. Pin; update both
   backends together; rerun the test list.
10. **One backend enough?** If B matches A on 2D tools in Phase 5, hide A in the UI (wrapper keeps
    it) to halve maintenance.

## Phase 1 artifacts (draft)
Checked 2026-10-20, local only (nothing published). Downloads hashed in
`~/DroidDesk-build/wine-artifacts/`; sha256 compared with the GitHub API `digest` field (match = verified).

| Asset | Upstream URL | Size | sha256 | Licence | Status |
|---|---|---|---|---|---|
| `hangover_11.16_debian13_trixie_arm64.tar` | https://github.com/AndreRH/hangover/releases/download/hangover-11.16/hangover_11.16_debian13_trixie_arm64.tar | 293099520 | `b5493f5903ab3c05f78bf4c03b06ac0005eb26f732437748c1592fefa45e861a` | LGPL-2.1+ (Wine), MIT (FEX, Box64), zlib (DXVK) | downloaded, verified |
| Hangover source, tag `hangover-11.16` (no submodules) | https://codeload.github.com/AndreRH/hangover/tar.gz/refs/tags/hangover-11.16 | small (no Content-Length) | pending | LGPL/MIT | pending |
| Wine source, AndreRH/wine `3ea3a14941b6a122b892c84141ce9662c008081e` (Hangover submodule, `VERSION` = 11.16) | https://codeload.github.com/AndreRH/wine/tar.gz/3ea3a14941b6a122b892c84141ce9662c008081e | ~50 MB [u] | pending | LGPL-2.1+ | pending |
| FEX source, AndreRH/FEX `fa556167d5a64ec7adb5503c2aa15b169c292cac` | https://codeload.github.com/AndreRH/FEX/tar.gz/fa556167d5a64ec7adb5503c2aa15b169c292cac | [u] (FEX's own submodules not included) | pending | MIT | pending |
| Box64 source, `2f130fab1d6e1a4ee8a71dc60cfdfcc839ad192a` (= tag v0.4.4 = Hangover submodule) | https://codeload.github.com/ptitSeb/box64/tar.gz/2f130fab1d6e1a4ee8a71dc60cfdfcc839ad192a | [u] | pending | MIT | pending |
| `wine-mono-11.3.0-x86.msi` (backend A, x86_64 Wine) | https://github.com/wine-mono/wine-mono/releases/download/wine-mono-11.3.0/wine-mono-11.3.0-x86.msi | 83348992 | `df2dfc1665c2511882e7cabd56eafd0c0a3d94e5a7e86f969277f6c189d418d3` | MIT + LGPL/MPL parts (Mono) | downloaded, verified (= `MONO_SHA` in Wine 11.16 `addons.c`) |
| `wine-mono-11.3.0-arm64.msi` (backend B, aarch64 Wine) | https://github.com/wine-mono/wine-mono/releases/download/wine-mono-11.3.0/wine-mono-11.3.0-arm64.msi | 89109504 | `b0c47d374efaf22cbff76cb10488ac2d1627b067e085aef7acec573884120e0f` | as above | downloaded, verified (= `MONO_SHA` for `__aarch64__`) |
| `wine-mono-11.3.0-src.tar.xz` | https://github.com/wine-mono/wine-mono/releases/download/wine-mono-11.3.0/wine-mono-11.3.0-src.tar.xz | 306872028 | `9f4f03285fbeed484980efbb1eba1c19069244aea401d2ff1585e3990782edf7` (API digest) | as above | not downloaded (>300 MB) |
| `box64-0.4.4-1-arm64.tar.xz` | `tools/box64/build.sh` (tag v0.4.4, commit pinned) | ~5–10 MB [u] | pending | MIT | script written, not built (no arm64 binfmt in Docker) |

Findings:
- Hangover tar = 4 debs + `dxvk-v2.7.1.tar.gz` (16475867 B). `hangover-wine 11.16~trixie`
  (Installed-Size 2429734 KiB ≈ **2.4 GB**, includes `/usr/include/wine` and the winegcc tools; the
  "~1 GB disk" estimate above is wrong), `hangover-libarm64ecfex`, `hangover-libwow64fex`,
  `hangover-wowbox64` (all 11.16, ~4–5 MB each, no Installed-Size field).
- All `hangover-wine` Depends exist in trixie arm64 (madison). `libavcodec61`/`libavformat61`/
  `libavutil59 (>= 7:7.1.5)` need the 13.x point release `7:7.1.5-0+deb13u1` — run
  `apt-get update && apt-get upgrade` before installing. Alternative dep `fex-emu-wine` is not in
  Debian (satisfied by `hangover-libarm64ecfex`).
- `hangover-wine` Conflicts `wine, wine-amd64, wine-i386`, Replaces/Provides `wine`, `wine32`,
  `wine64` and older names; does not mention `libwine` (Debian's lives in
  `/usr/lib/aarch64-linux-gnu/wine`, Hangover's in `/usr/lib/wine`) — no path clash expected, but
  `wine64-tools`/`libwine-dev` overlap (`/usr/include/wine`, `widl`, `winegcc`) is [u]. Install
  script: refuse if any Debian `wine*`/`libwine*` package is installed.
- Both FEX debs Conflict `fex-emu-wine`; install only `hangover-libarm64ecfex` +
  `hangover-libwow64fex` + `hangover-wowbox64` + `hangover-wine`.
- Wine 11.16 wants **wine-mono 11.3.0**; arm64 Wine asks for the `-arm64.msi`, x86_64 Wine for
  `-x86.msi` — mirror both. The plan's `WINE_MONO_VERSION`/`_SHA256` becomes one version, two hashes.
- LGPL source for Hangover is not attached to its release; mirror = tag archive + the three
  submodule commit archives (FEX additionally needs its `External/` submodules for a full source).

## Phase 2 status (2026-10-20, local, not committed)
Decision (Matěj): **v1 ships only backend B** = Hangover 11.16; x64 always `HODLL64=libarm64ecfex.dll`,
x86 emulator selectable `fex` (`HODLL=libwow64fex.dll`, default) or `box64` (`HODLL=wowbox64.dll`).
Backend A is not implemented; the wrapper keeps `--backend` / `~/.wine-<backend>` so it can be added.
- `app/android/app/src/main/assets/droiddesk/droiddesk-wine` (POSIX sh) and
  `.../assets/droiddesk/wine-install.sh` (bash) — in `assets/droiddesk/` next to `droiddesk-usb.py`,
  so Kotlin reads them as `droiddesk/<name>` like the other tools.
- Wrapper: commands per "Common design" minus `backend` (replaced by `x86`): `x86 fex|box64` writes
  `X86_EMU` in `/etc/droiddesk/wine.conf`; `x86 fex|box64 APP` / `x86 default APP` edit
  `~/.config/droiddesk-wine/apps` (key = lower-case `.exe`/`.lnk` file name). Precedence:
  `--x86` > `DROIDDESK_WINE_X86` > apps file > wine.conf > fex. Locale: `LOCALE=` in wine.conf (the
  installer's `--locale`, from the app's `Locale.getDefault()`), else Android `getprop` if it runs in
  proot [u], else en_US; `cs*` → `LANG=LC_ALL=cs_CZ.UTF-8`. gpu-env.sh is sourced unless
  `/etc/droiddesk/gpu-off`. First run: `wineboot -i` with `mscoree=d;mshtml=d` (no Mono/Gecko
  dialog), D: → `/storage/emulated/0`, COM3/COM4 links, then `msiexec /i` our arm64 Mono MSI
  (re-run when the MSI name changes). COM: dosdevices symlink **and** `HKLM\Software\Wine\Ports`
  value [u: which one mountmgr honours under proot]. Paths: `/sdcard/…` and Termux-side paths into the
  Debian rootfs are mapped, other `/data/…` refused with a Czech hint. After every app: `wineserver -w`,
  then Start Menu sync (`wine-fex-*.desktop`, uninstall shortcuts skipped, generic icon) +
  `droiddesk-menu-sync`.
- Installer: `wine-install.sh --hangover URL SHA256 --mono URL SHA256 [--version L] [--locale ll_CC]
  [--termux-home DIR] [--slim] [--force]`; refuses Debian `wine*`/`libwine*`/`fex-emu-wine` (lists
  them); disk check 6 GB fresh / 1.2 GB upgrade; explicit `apt-get update` + `apt-get -y upgrade`
  step; download + `sha256sum -c`; `hangover.new` → apt install of the 4 debs (+ fonts-liberation2,
  fonts-dejavu-core) → swap; debs deleted afterwards, DXVK tarball kept in `/opt/droiddesk-wine/dxvk`;
  Mono MSI in `/opt/droiddesk-wine/mono`; wine.conf keeps `X86_EMU`/`LOCALE` on reinstall; three
  .desktop files; MIME defaults in `/etc/xdg/mimeapps.list` and the Termux `~/.config/mimeapps.list`
  (path from `--termux-home` or droiddesk-menu-sync's `OUT=`). Same sha256 already installed = only
  config is rewritten. Log `/var/log/droiddesk-wine-install.log`.
- `--slim`: dpkg `path-exclude` for `/usr/include/wine`, the `*.a` import libraries and the
  winegcc/widl/wrc/winebuild/winedump/winemaker/wmc tools — identified from the deb file list, saves
  only ~128 MB of 2.4 GB. The bulk is PE DLLs (aarch64-windows 1.71 GB, i386-windows 0.54 GB;
  aarch64 `ntdll.dll` 7.3 MB) — probably unstripped [u]; stripping would be the real saving (Phase 5).
- Harness `tools/wine-test/run.sh` (Docker `debian:trixie` x86_64, stubbed wine/wineserver/apt-get/
  dpkg-query): sh/dash/bash `-n`, shellcheck, LF, parsing, path refusal, first run, HODLL precedence,
  locale/debug/gpu env, file types, COM, menu sync, doctor, uninstall, install + rerun + `--force` from
  file:// artifacts — **94 checks, WINE_ALL_OK**.
- Phase 3 (Kotlin): copy both assets to `/usr/local/share/droiddesk/` in Debian; install =
  `debian-run bash /usr/local/share/droiddesk/wine-install.sh --hangover … --mono … --locale
  <lang_COUNTRY> --termux-home <homeDir>`; status = `usr/local/bin/droiddesk-wine` + `etc/droiddesk/wine.conf`.
- Phone test still open: real install (apt upgrade size/time, 2.4 GB), first run + Mono, Czech text,
  `getprop` in proot, COM via Ports key vs symlink, Thunar MIME type of `.exe` on the Termux side
  (`application/x-msdownload` vs `vnd.microsoft.portable-executable`), Start Menu sync after an installer.
