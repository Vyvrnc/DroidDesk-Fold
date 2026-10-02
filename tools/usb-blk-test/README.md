# droiddesk-usb format/files test harness

Runs the real tools (mkfs.fat, mkfs.exfat, mkfs.ext4, debugfs, mtools) through
`libdroiddesk_blk.so` (built for glibc) against a fake UsbBridge that serves `blk`
from an image file, in Debian arm64:

    docker run --rm -v "$PWD/tools/usb-blk-test:/t" -v "$PWD/app/android/app/src/main:/src:ro" \
        debian:trixie bash /t/run.sh        # format + files, fsck after each step; ends with ALL_OK
    docker run ... debian:trixie bash /t/gui.sh   # USB disky window under Xvfb, screenshot /t/gui.png

`BS=4096 SIZE=2G` for other block sizes and disk sizes.
