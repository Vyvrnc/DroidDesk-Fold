# "USB disky" — plan (2026-10-02)

Matěj wants a panel icon for USB flash drives and card readers in the DroidDesk desktop: list with
[drženo], attach to Linux / eject, write and save images, format, and files on the disk — without root.

What exists (fold.15): `droiddesk-usb list|flash|read|eject|exec` over UsbBridge; devices stay held in
Linux until eject (UsbBridge.HeldDevice); BotScsiDevice (BOT/SCSI) does block I/O.

Constraint: no root means no `/dev/sdX`, no kernel filesystems and no FUSE. Everything that touches the
disk goes through the app (UsbBridge) or through user space programs that we point at it.

## Phase 1 — panel icon, attach/eject, images  → fold.16 (done)
Built with Python + GTK 3 (pygobject, `droiddesk-usb-tray.py`) instead of yad: Termux's yad 15
depends on webkit2gtk-4.1 (100+ MB) for one icon. Plugin 27 = systray in the dock.
- `droiddesk-usb attach [DEVICE]`: permission + hold without an operation (bridge `attach <name>`).
- Bridge `watch`: streams "attached/detached/held/released" events (USB_DEVICE_ATTACHED/DETACHED
  broadcasts + hold changes), so the icon needs no polling.
- `droiddesk-usb-tray` (Termux side, bash + yad): `yad --notification` in the XFCE panel (systray
  plugin added to the dock by droiddesk-xfce-tweaks). Left click: window with the device list
  (vendor/product, size, [drženo]) and buttons Připojit do Linuxu / Vysunout / Zapsat obraz… /
  Uložit obraz… . A new device → notification "Připojit do Linuxu?".
- Image write/save: yad file dialog → `droiddesk-usb flash|read --yes` with a yad progress window
  fed from the client's progress lines (client gets `--progress-fd`/machine-readable mode).
- Menu entry "USB disky" (Systém).

## Phase 2 — the held device as a file for user space tools → fold.18
Implementation (2026-10-02):
- Bridge `blk <name> <lun|->`: text line `device <capacity> <bs> <slot> <n>`, then binary requests
  `op(1) lba(8, BE) count(4, BE) [data]` with op R/W/F/Q; reply `status(1)` (+ data for R, or
  `len(2) message` on error). One session holds the device lock until the socket closes.
- `libdroiddesk_blk.so` (NDK, bionic; built in the APK, copied to `$PREFIX/lib`): `/dev/droiddesk-blk`
  is the disk (or a partition: `DROIDDESK_BLK_OFFSET/SIZE`), reported as a regular file of that size.
- Tools from Termux: dosfstools, exfatprogs, e2fsprogs (mkfs + debugfs for ext4 files), mtools (FAT).
- `droiddesk-usb info|format|ls|cp|rm|mkdir`; tray: columns Kapacita / Systém souborů / Jmenovka,
  buttons Naformátovat… and Soubory….
- Local test harness: fake bridge over an image file + glibc build of the shim in Docker.

Original plan:
- `libdroiddesk-blk.so` (glibc, Debian; LD_PRELOAD shim): intercepts open/pread/pwrite/lseek/
  fstat/ioctl(BLKGETSIZE64)/fsync on `/dev/droiddesk-usb/<bus>-<dev>[/lun]` and forwards them to a new
  bridge request `blk <name> <lun>` (random-access sector reads/writes over the socket, block aligned,
  with a small cache for partial blocks). Other paths pass through untouched.
- With it, unmodified tools work on the disk: `sfdisk`, `mkfs.vfat`, `mkfs.exfat`, `mkfs.ext4`,
  `fsck.*`, `mtools` (mdir/mcopy/mdel/mmd on FAT), `e2tools`/`debugfs` (ext2/3/4).
- `droiddesk-usb format DEVICE fat32|exfat|ext4 [LABEL]`: MBR with one partition (sfdisk), mkfs on
  the partition (offset), wipe old GPT backup at the end. Confirmation like flash.
- `droiddesk-usb ls|cp|rm|mkdir` wrappers: FAT via mtools, ext4 via e2tools; exFAT read-only unless
  a user space library turns up.

## Phase 3 — files from Thunar → fold.18+
- Debian: small WebDAV server on 127.0.0.1 per held device (wsgidav with a provider over PyFilesystem2;
  FAT32 via pyfatfs, ext4 via an e2tools/debugfs backed provider), started on attach, stopped on eject.
- Thunar opens it through gvfs (`davs://`/`dav://localhost:<port>`; Termux gvfs has http/sftp, no fuse);
  the tray adds a bookmark "USB: <label>".
- Writes go through the same shim, so eject flushes (SYNCHRONIZE CACHE) before release.

## Risks / open
- Cheap sticks freeze under load (hub power, see fold.14) — the GUI must show errors plainly and offer
  eject/replug.
- exFAT file access has no mature user space implementation; FAT32 and ext4 first.
- WebDAV over a slow USB2 hub: fine for documents, slow for big copies (CLI `cp` is faster).
