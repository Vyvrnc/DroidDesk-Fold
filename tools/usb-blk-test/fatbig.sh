#!/bin/bash
# FAT32 on a large (sparse) disk: nested mkdir on a fresh filesystem.
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq python3 gcc libc6-dev mtools dosfstools >/dev/null 2>&1
P=/tmp/prefix; mkdir -p $P/bin $P/lib $P/tmp
gcc -shared -fPIC -O2 -o $P/lib/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
for t in mkfs.fat mdir mcopy mdel mdeltree mmd mrd; do ln -sf "$(command -v $t)" "$P/bin/$t"; done
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
truncate -s ${SIZE:-128G} /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 & sleep 1
$CLI format fat32 --label BIG --yes | tail -1
$CLI info
$CLI mkdir usb:/a/b/c; echo "mkdir rc=$?"
$CLI ls usb:/a; $CLI ls usb:/a/b
printf 'drive u: file="/dev/droiddesk-blk"\nmtools_skip_check=1\n' > /tmp/rc
export MTOOLSRC=/tmp/rc LD_PRELOAD=$P/lib/libdroiddesk_blk.so DROIDDESK_BLK_DEVICE=/dev/bus/usb/001/002 DROIDDESK_BLK_OFFSET=1048576
mmd u:/x; echo "mmd x rc=$?"; mmd u:/x/y; echo "mmd x/y rc=$?"; mdir -a u:/x
