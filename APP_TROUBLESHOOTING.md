# App Troubleshooting Guide

Welcome to the DroidDesk troubleshooting guide. This document explains how to fix common issues when installing and running Linux applications inside the DroidDesk container.

## This fork: desktop and Debian

**Black screen with a mouse cursor after restarting the desktop.** X is running but XFCE did not start, usually because a PulseAudio left over from the previous session stopped answering. Fixed in 1.0.1-fold.5; on older builds run `pkill -9 pulseaudio` in DroidDesk's own terminal.

**Stuck on "Almost ready" after the phone wakes up.** Since 1.0.1-fold.5 the X connection is retried every 20 s and the desktop is shown after 100 s at the latest. If it still happens, switch to another app and back.

**The whole desktop disappears after minimizing.** Android killed the X server process under memory pressure (`adb shell dumpsys activity exit-info com.orailnoor.droiddesk` shows `:x11 … LOW_MEMORY`). Since 1.0.1-fold.4 it keeps the session's priority. Large apps (slicers, browsers with many tabs) still need free RAM; close Android apps you do not need.

**`proot-distro login debian` fails with "can't create temporary directory".** Use `start-debian` (or `debian-run`, `claude-debian`): they set `PROOT_TMP_DIR` and the loader for DroidDesk's prefix.

**"CANNOT LINK EXECUTABLE … libandroid-support.so" inside Debian.** A Termux program was found on Debian's `PATH`. Run `debian-setup` once; it removes the Termux bin directory from Debian's `PATH`.

**Native Linux binaries fail with "required file not found" (Claude Code, Brave, AppImages).** They need glibc; Termux uses Android's Bionic. Run them in the Debian container (`start-debian`, then install there).

**No sound from Debian programs.** Debian talks to PulseAudio over TCP on 127.0.0.1. Update to 1.0.1-fold.5+ and restart the desktop; `pactl info` (package `pulseaudio-utils`) in Debian should show a sink.

**3D apps are slow or crash in Debian.** `droiddesk-gpu status` shows whether hardware OpenGL is on. If an app crashes with it, try `droiddesk-gpu off` (software rendering) and report the app. `glxgears` itself crashes with hardware OpenGL since 1.0.1-fold.7 (zink without kopper); use another test app. GL windows flickering on older builds: run `debian-setup` to get Mesa revision 26.2.3-2.

**Links open in an Android browser instead of Firefox.** Since 1.0.1-fold.6 `droiddesk-open` and `xdg-open` use the Linux default app from `mimeapps.list` first. Set it with `xdg-mime default firefox.desktop x-scheme-handler/https` if needed.

**`droiddesk-usb` says "several cards inserted".** Multi-slot card readers expose one slot per LUN; choose the card with `--lun N` (the error lists the slots and sizes). Android asks for USB permission the first time; accept it on the phone.

## The "Running as root without --no-sandbox is not supported" Error

Because DroidDesk utilizes a PRoot environment, you are logged in as the `root` user by default. Modern Electron and Chromium-based applications have strict security sandboxes that refuse to run as root.

Apps known to have this issue include:
- Visual Studio Code
- Google Chrome / Chromium
- Discord
- Brave Browser
- Microsoft Edge
- Obsidian
- Cursor

### 1. Automatic Fixes (Recommended)
DroidDesk includes an **Auto-Patcher** that automatically fixes these applications. It hooks into the package manager so you don't have to do anything manually.

If you install an app using:
- `apt install <package>`
- `dpkg -i <package>.deb`

The auto-patcher will instantly and silently patch the app in the background so it launches perfectly from the desktop menu.

### 2. Manual Fixes (For Tarballs and Standalone Binaries)
If you download an application as a standalone binary (e.g., a `.tar.gz`, an `.AppImage`, or extracting a zip file) without using `apt` or `dpkg`, the auto-patcher will **not** trigger automatically. 

If your manually installed application refuses to launch, follow these steps:

#### Method A: Run the Global Patch Script
If you manually copied the application into a system directory (like `/usr/share` or `/opt`), you can run the auto-patcher manually:

```bash
/usr/local/bin/patch-root-binaries.sh
```
This script will scan standard installation directories, rename the real binary, and create a transparent shell wrapper in its place that automatically passes the `--no-sandbox` flag.

#### Method B: Manual Terminal Wrapper (For Custom Directories)
If you extracted the app into a custom folder (e.g., `~/Downloads/My-App`), you can create a simple bash wrapper yourself so you don't have to type the flag every time:

```bash
# Rename the real binary
mv ~/Downloads/My-App/app ~/Downloads/My-App/app.real

# Create a wrapper script in its place
cat << 'EOF' > ~/Downloads/My-App/app
#!/bin/bash
exec ~/Downloads/My-App/app.real --no-sandbox "$@"
EOF

# Make it executable
chmod +x ~/Downloads/My-App/app
```

#### Method C: Launching from the Terminal
If you are launching the standalone binary directly from the terminal, simply pass the flag manually when you run the command:
```bash
./my-electron-app --no-sandbox
```

## "VLC is not supposed to be run as root" Error

VLC Media Player also has a hardcoded block preventing it from running as root. 
If you install VLC via `apt`, our auto-patcher handles it. 

If you compile or install VLC manually, you can bypass the root check by patching the binary with this command:
```bash
sed -i 's/geteuid/getppid/g' /path/to/your/vlc/binary
```
