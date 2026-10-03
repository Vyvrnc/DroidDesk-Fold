#!/bin/bash
# Copy progress lines (percent, speed, time left) for each filesystem, up and down.
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 mtools dosfstools exfatprogs e2fsprogs ntfs-3g >/dev/null 2>&1
P=/tmp/prefix; mkdir -p $P/bin $P/lib $P/tmp
gcc -shared -fPIC -O2 -o $P/lib/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
E=/src/cpp/exfat; gcc -shared -fPIC -O2 -w -I$E/libexfat -DPACKAGE='"droiddesk-exfat"' -DVERSION='"1.4.0"' -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -o $P/lib/libdroiddesk_exfat.so $E/exfat_tool.c $E/libexfat/*.c
N=/src/cpp/ntfs; gcc -shared -fPIC -O2 -w -I$N -I$N/include/ntfs-3g -DHAVE_CONFIG_H -o $P/lib/libdroiddesk_ntfs.so $N/ntfs_tool.c $N/libntfs-3g/*.c
for t in mkfs.fat mkfs.exfat mkfs.ext4 mkntfs debugfs mdir mcopy mdel mdeltree mmd mrd python3; do ln -sf "$(command -v $t)" "$P/bin/$t"; done
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
truncate -s 1G /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 & sleep 1
mkdir -p /tmp/up/d; for i in 1 2 3 4 5 6; do head -c 6000000 /dev/urandom > /tmp/up/d/f$i.bin; done
for fs in fat32 ext4 exfat ntfs; do
  $CLI format $fs --yes >/dev/null
  echo "== $fs up"; $CLI cp /tmp/up/d usb:/d --yad | grep -E "^# Kopíruji" | sed -n '1p;$p'
  rm -rf /tmp/back; mkdir /tmp/back
  echo "== $fs down"; $CLI cp usb:/d /tmp/back --yad | grep -E "^# Kopíruji" | sed -n '1p;$p'
  diff -r /tmp/up/d /tmp/back/d && echo "ok: $fs identical" || echo "FAIL: $fs"
done
