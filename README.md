# DroidDesk

> [!NOTE]
> **This is a modified fork** of [techjarves/DroidDesk-Enhanced](https://github.com/techjarves/DroidDesk-Enhanced) v1.0.1
> (itself based on [orailnoor/DroidDesk](https://github.com/orailnoor/DroidDesk)), tuned for the Galaxy Z Fold 7 and Samsung DeX.
> Changes, all in the `fold-dex` history:
>
> - Per-screen desktop scale (inner, cover, monitor/DeX) with an on-screen toggle; keeps the desktop out of the camera cutout
> - DeX: the Linux session survives moving between the phone and DeX; the Meta key goes to XFCE
> - Shared storage: runtime permission, `~/storage/*` links and Thunar bookmarks
> - `socket_hook`: rewrites `bind()`/`connect()` Unix socket paths (fixes LibreOffice hanging and restarting in a loop)
> - Opening files with default apps (`GIO_LAUNCH_DESKTOP`), progress of the Debian install, single XFCE dock instead of overlapping panels
>
> **Stability**
> - The X server process (`:x11`) keeps the session's priority, so Android no longer kills it under memory pressure after minimizing
> - A stale PulseAudio from a previous session can no longer block the desktop start (black screen with a cursor)
> - The loading screen retries the X connection and no longer hangs on "Almost ready" after the phone wakes up
> - `socket_hook` also redirects `execve()` of compiled-in `com.termux` paths (e.g. bash's command-not-found handler)
>
> **Debian container** (proot, for glibc programs such as the native Claude Code build)
> - `start-debian`, `debian-run CMD`, `claude-debian [args]` (Claude Code in `~/projekty/Claude`)
> - `debian-setup` (run once, idempotent): basic tools, Android time zone and locales, Android fonts (emoji, CJK), Debian apps in the XFCE menu, faster apt, Termux binaries removed from Debian's `PATH`
> - Hardware OpenGL through Zink on Turnip/KGSL (Adreno), a separate Mesa build in `/opt/mesa-kgsl` ([release asset](../../releases/tag/mesa-kgsl-26.2.3), build notes in `tools/mesa-kgsl`); `droiddesk-gpu on|off`, `gpu-run CMD`
> - Sound (PulseAudio over TCP on 127.0.0.1), the XFCE session bus (notifications), OTG drives under `/storage`, DNS taken from Android's active network (follows a VPN)
>
> **Desktop and Android integration**
> - `droiddesk-open [--android] FILE|URL` opens with the Linux app from `mimeapps.list` (Firefox, Mousepad, …) and falls back to an Android app only when there is none; `--android` always uses Android. Also behind `termux-open`, Debian's `xdg-open` and Thunar's "Otevřít v Androidu"
> - **USB disky**: an icon in the dock (systray) and a menu entry — lists flash drives and card readers, attaches them to Linux / ejects them, writes and saves images with a progress window, and offers "Připojit do Linuxu" when a drive is plugged in. Columns show capacity, filesystem and label; **Naformátovat…** (FAT32, exFAT, NTFS, ext4) and **Soubory…** (browse, upload, download, new folder, delete on FAT, exFAT, NTFS and ext2/3/4). Without root this works through `libdroiddesk_blk.so`, an `LD_PRELOAD` shim that lets dosfstools, exfatprogs, ntfs-3g (mkntfs), e2fsprogs (debugfs) and mtools use the disk held by the app; exFAT and NTFS files go through small batch tools on libexfat and libntfs-3g built into the APK. NTFS left "in use" by Windows (hibernation, fast startup) is only read, never written. Needs `pygobject` (installed with XFCE from fold.16; older installs are offered the install on first use).
> - `droiddesk-usb`: flash drives and card readers without root — `flash IMAGE[.xz|.gz]` (compressed images are fully checked before anything is written, then verified by reading back), `read FILE`, `--lun N` for multi-slot card readers, and an experimental `exec DEVICE -- CMD` that hands a libusb program the usbfs descriptor. The SCSI/Bulk-Only layer follows [EtchDroid](https://github.com/EtchDroid/EtchDroid) and [libaums](https://github.com/magnusja/libaums)
> - Battery level in the dock, Android apps removed from the dock stay removed, windows tile at screen edges (Super+arrows), no xfwm4 compositor
>
> APKs are on the [Releases](../../releases) page and can be tracked with [Obtainium](https://github.com/ImranR98/Obtainium).
> After updating an existing install, run `debian-setup` once in the terminal.

Run a full Linux desktop on any Android phone. Not a terminal. Not an emulator. A complete desktop environment with direct kernel access -- VS Code, Blender, Metasploit, local AI, all of it.

Connect your phone to a monitor and it becomes a Linux PC. Unplug it and your entire setup comes with you.

> [!IMPORTANT]
> DroidDesk is an independent GPL-3.0 open-source project that incorporates
> modified Termux:X11 components. It is not affiliated with or endorsed by
> Termux, Termux:X11, TUR, Canonical, or Ubuntu.
>
> - **Source and licenses:** <https://github.com/orailnoor/DroidDesk>
> - **Termux:X11 upstream:** <https://github.com/termux/termux-x11>

## Video

[![Watch the video](https://img.youtube.com/vi/QCr4WWsfVv8/maxresdefault.jpg)](https://youtu.be/QCr4WWsfVv8)

## What This Actually Runs

Everything below has been tested and confirmed working:

- **LibreOffice** -- Word processing, spreadsheets, presentations. Fully functional.
- **VS Code** -- Full version. Python, PIP, extensions, everything.
- **Claude Code** -- AI coding agent running directly in terminal.
- **Blender** -- Installs and opens. Laggy on mobile hardware, but it runs.
- **Wireshark** -- Full network analysis, every packet and protocol.
- **Metasploit** -- Pentesting framework, runs fine.
- **Local AI** -- Offline LLM inference, 5+ tokens/second, no API needed.

If it runs on Ubuntu, it runs here.

## How It Works

The Linux environment runs through Termux with direct access to the phone's kernel. No emulation, no translation -- native performance.

The setup script installs a full desktop (XFCE4/LXQt/MATE/KDE) inside Termux using the Termux User Repository (TUR) for GUI apps. For tools not available in TUR (Wireshark, Metasploit, etc.), a Proot container provides a standard Ubuntu/Debian/Kali environment where you install anything with `apt`.

The automatic menu sync scans what you install inside Proot and adds it directly to your desktop app menu. No need to enter the container every time.

## DroidDesk App (Standalone)

DroidDesk is also available as a standalone Android application that completely automates this process without requiring a separate Termux installation. It renders through an embedded Termux:X11 server running in its own Android process; the app does not use VNC.

- **Rooted phones:** Run the Ubuntu filesystem through `chroot`.
- **Non-rooted phones:** Run an app-private native Termux userspace and install desktop packages from the X11 and TUR repositories. PRoot is not used.
- **Rendering:** Both modes connect directly to the embedded X11 server on `DISPLAY=:0`. Adreno devices use Turnip/Zink hardware acceleration when available; other GPUs fall back to Mesa software rendering.
- **Automated setup:** The app extracts the bundled ARM64 Termux bootstrap, configures its private package prefix, and installs the selected desktop automatically.

Download the latest release APK from the Releases tab and sideload it to begin.

## Requirements

- Any Android phone (ARM64)
- [Termux](https://f-droid.org/en/packages/com.termux/) (install from F-Droid, not Play Store)
- [Termux-X11](https://github.com/termux/termux-x11/releases/tag/nightly) (for on-phone display)

### For Monitor Output ( Optional )

**Option A: USB-C Display Output**
If your phone supports display output over USB-C, just use a USB-C to HDMI adapter. Done.

**Option B: Raspberry Pi Bridge**
For phones without display output (most mid-range phones with USB 2.0), use a Raspberry Pi Zero 2W as a bridge:
- Raspberry Pi Zero 2W with Raspberry Pi OS
- Micro USB to USB-C cable
- USB-C hub
- Micro HDMI to HDMI adapter
- SD card with Pi firmware
- Wireless keyboard and mouse

The Pi connects to the phone via USB tethering, detects the phone's IP automatically, and opens a VNC viewer to display the phone's desktop on the monitor.

## Installation

### Step 1: Install Termux

Download and install Termux from F-Droid:
https://f-droid.org/en/packages/com.termux/

Do NOT use the Play Store version. It is outdated and will not work.

### Step 2: Install Termux-X11

Download the latest APK from:
https://github.com/termux/termux-x11/releases/tag/nightly

Install it on your phone. This is the display server that renders the desktop.

### Step 3: Run the Setup Script

Open Termux and run:

```bash
curl -sL https://raw.githubusercontent.com/orailnoor/DroidDesk/main/termux-linux-setup.sh -o setup.sh
bash setup.sh
```

The script will:
1. Update Termux packages
2. Add X11 and TUR repositories
3. Install your chosen desktop environment (XFCE4/LXQt/MATE/KDE)
4. Set up GPU acceleration (Turnip for Adreno, Zink fallback for others)
5. Install Firefox, Git, Python, and core tools
6. Set up a Proot Linux container (Ubuntu/Debian/Kali)
7. Create the App Bridge for automatic menu syncing
8. Apply a modern dark theme
9. Optionally set up VNC for remote access

### Step 4: Start the Desktop

After installation completes:

```bash
bash ~/start-x11.sh
```

Then open the Termux-X11 app on your phone. Your desktop is ready.

### Step 5: Install Apps Inside Proot

To install tools that are not in TUR:

```bash
bash ~/start-proot.sh
apt install wireshark    # or any other package
exit
bash ~/proot-menu-sync.sh
```

The app will appear in your desktop menu automatically.

## Raspberry Pi Monitor Bridge Setup

If you are using a Raspberry Pi Zero 2W to output to a monitor:

### Step 1: Flash Raspberry Pi OS

Flash standard Raspberry Pi OS to an SD card and boot the Pi.

### Step 2: Install VNC Viewer on the Pi

```bash
sudo apt update
sudo apt install realvnc-vnc-viewer
```

### Step 3: Copy the Launcher Script

Copy `pi-launch_phone.sh` to your Pi:

```bash
curl -sL https://raw.githubusercontent.com/orailnoor/DroidDesk/main/pi-launch_phone.sh -o ~/pi-launch_phone.sh
chmod +x ~/pi-launch_phone.sh
```

### Step 4: Connect and Launch

1. Connect the phone to the Pi via USB cable
2. Enable USB Tethering on the phone
3. Start VNC on the phone: `bash ~/start-vnc.sh` (in Termux)
4. Run the bridge script on the Pi:

```bash
bash ~/pi-launch_phone.sh
```

The script auto-detects the phone's IP and opens a fullscreen VNC session on the monitor.

### Optional: Auto-Launch on Boot

To make the Pi automatically connect when powered on, add to crontab:

```bash
crontab -e
```

Add this line:

```
@reboot sleep 15 && /home/pi/pi-launch_phone.sh
```

## Commands Reference

| Command | What It Does |
|---|---|
| `bash ~/start-x11.sh` | Start desktop via Termux-X11 |
| `bash ~/start-vnc.sh` | Start desktop via VNC (if installed) |
| `bash ~/start-proot.sh` | Open Proot Linux shell |
| `bash ~/proot-menu-sync.sh` | Sync Proot apps to desktop menu |
| `bash ~/stop-linux.sh` | Stop all sessions |

Commands of this fork's DroidDesk app (in its terminal):

| Command | What It Does |
|---|---|
| `start-debian` | Shell in the Debian container (use it instead of plain `proot-distro login`) |
| `debian-run CMD…` | Run one Debian program from the Termux side |
| `claude-debian [args]` | Claude Code inside Debian, in `~/projekty/Claude` (e.g. `claude-debian --resume`) |
| `debian-setup` | Install/refresh the Debian integration (tools, locale, time zone, fonts, menu, GPU); safe to repeat |
| `droiddesk-gpu on\|off\|status` | Hardware OpenGL for Debian programs (applies to newly started apps) |
| `gpu-run CMD…` | One Debian program with hardware OpenGL even while it is switched off |
| `droiddesk-open [--android] FILE\|URL` | Open with the Linux app, or in Android with `--android` / when no Linux app fits |
| `droiddesk-usb list` | USB devices attached to the phone |
| `droiddesk-usb flash IMAGE[.xz\|.gz] [DEVICE] [--lun N]` | Write a disk image to a flash drive or card and verify it |
| `droiddesk-usb read FILE [DEVICE] [--lun N]` | Copy a whole flash drive or card into a file |
| `droiddesk-usb attach [DEVICE]` | Take a flash drive or card into Linux now (asks for the USB permission) |
| `droiddesk-usb eject [DEVICE]` | Give a flash drive or card back to Android after `read`/`flash` (they keep it in Linux until then) |
| `droiddesk-usb watch` | Print attach/detach/hold events as they happen |
| `droiddesk-usb info [DEVICE]` | Capacity, partitions, filesystems and labels of a held disk |
| `droiddesk-usb format [DEVICE] fat32\|exfat\|ntfs\|ext4 [--label NAME]` | New MBR with one partition and a filesystem (asks first) |
| `droiddesk-usb check [DEVICE] [--write]` | Read the whole disk twice and compare (unstable memory); `--write` fills it with a pattern and verifies (fake capacity, bad sectors — erases it) |
| `droiddesk-usb ls [usb:/PATH]` | Files on the disk (FAT, exFAT, NTFS, ext2/3/4) |
| `droiddesk-usb cp FILE… usb:/FOLDER` / `cp usb:/PATH… TARGET` | Copy to / from the disk, folders recursively |
| `droiddesk-usb rm [-r] usb:/PATH…`, `mkdir usb:/PATH…` | Delete, create folders on the disk |
| `droiddesk-usb mv usb:/A usb:/B` | Rename or move on the disk (replaces a file like `mv`) |
| `droiddesk-usb-dav DEVICE [--secret]` | WebDAV server for Thunar (`dav://localhost:PORT/…`); the USB disky window starts it for every disk held by Linux and adds a Thunar bookmark "USB – …" |
| `droiddesk-usb-tray --window` | The "USB disky" window (also in the menu and as an icon in the dock) |
| `droiddesk-usb exec DEVICE -- CMD…` | Experimental: give a libusb program the USB device (`DROIDDESK_USB_FD`) |

## Notes

> [!WARNING]
> **Disable Child Process in Developer Options**
> On some Android versions (MIUI, One UI, stock Android 13+), the system may kill Termux background processes and drop your desktop session. To prevent this:
> 1. Go to **Settings → Developer Options**
> 2. Find **"Child process"** (may be labeled differently depending on your ROM)
> 3. Disable child process restrictions for Termux
>
> Without this, long-running sessions (VNC, Termux-X11) may be killed by the OS without warning.

- Termux-X11 directly on the phone is faster than VNC. Use VNC only when you need monitor output through the Pi bridge or remote access from another device.
- For standalone phone use without a monitor, Termux-X11 is the recommended option.
- The Proot container shares the display with the native Termux desktop. Apps installed in Proot render on the same screen.
- GPU acceleration works best on Adreno GPUs (Qualcomm Snapdragon phones). Other GPUs fall back to software rendering.

## Credits

Created by [orailnoor](https://youtube.com/@orailnoor)

## License and third-party software

DroidDesk is independent software licensed under
[GNU GPL version 3 only](LICENSE). It is not affiliated with or endorsed by
Termux, Termux:X11, TUR, Canonical, Ubuntu, or other upstream projects.

The Android application incorporates GPL-licensed Termux:X11 components and
bundles other third-party software under their respective licenses. See:

- [Notices and attribution](NOTICE.md)
- [Third-party software inventory](THIRD_PARTY_NOTICES.md)
- [Release compliance status](COMPLIANCE.md)

The current compliance checklist includes unresolved source provenance,
reproducible-build, custom-prefix bootstrap, and wallpaper-license work. Do not
describe a binary release as fully compliant until the blocking checklist items
are complete.
