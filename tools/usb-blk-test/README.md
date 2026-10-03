# droiddesk-usb format/files test harness

Runs the real tools (mkfs.fat, mkfs.exfat, mkfs.ext4, debugfs, mtools) through
`libdroiddesk_blk.so` (built for glibc) against a fake UsbBridge that serves `blk`
from an image file, in Debian arm64:

    docker run --rm -v "$PWD/tools/usb-blk-test:/t" -v "$PWD/app/android/app/src/main:/src:ro" \
        debian:trixie bash /t/run.sh        # format + files, fsck after each step; ends with ALL_OK
    docker run ... debian:trixie bash /t/gui.sh   # USB disky window under Xvfb, screenshot /t/gui.png

`BS=4096 SIZE=2G` for other block sizes and disk sizes.

    docker run ... debian:trixie bash /t/ntfs_test.sh     # NTFS tool in depth (dirty/hibernated volumes)
    docker run ... debian:trixie bash /t/concurrency.sh   # parallel commands, killed client, debug mode, dup/fork
    docker run ... debian:trixie bash /t/serial.sh        # droiddesk-serial against fake_serial_bridge.py (simulated
                                                          # adapter: Modbus RTU slave at 19200 8E1, echo at 115200 8N1);
                                                          # mbpoll + pyserial through the pty; ends with SERIAL_ALL_OK
    docker run ... debian:trixie bash /t/gui_serial.sh    # serial adapters in the USB window, /t/gui_serial.png
    docker run ... debian:trixie bash /t/dav.sh           # droiddesk-usb mv + droiddesk-usb-dav (WebDAV, curl) on
                                                          # fat32/ext4/exfat/ntfs, fsck after each; GVFS=1 adds gio
                                                          # (gvfs dav backend, as Thunar); ends with DAV_ALL_OK
