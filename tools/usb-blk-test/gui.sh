#!/bin/bash
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 python3-gi gir1.2-gtk-3.0 xvfb mtools dosfstools exfatprogs e2fsprogs adwaita-icon-theme fonts-dejavu-core >/dev/null || exit 1
gcc -shared -fPIC -O2 -o /tmp/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
P=/tmp/prefix; mkdir -p $P/bin $P/lib $P/tmp; cp /tmp/libdroiddesk_blk.so $P/lib/
for t in python3 mkfs.fat mkfs.exfat mkfs.ext4 debugfs mdir mcopy mdel mdeltree mmd mrd; do ln -s "$(command -v $t)" "$P/bin/$t"; done
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
truncate -s 2G /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 &
sleep 1
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
$CLI format fat32 --label FOLD --yes >/dev/null
echo ahoj > /tmp/a.txt; $CLI mkdir usb:/docs/Fotky; $CLI cp /tmp/a.txt usb:/docs >/dev/null
Xvfb :9 -screen 0 1400x700x24 >/dev/null 2>&1 &
sleep 2
DISPLAY=:9 timeout 60 python3 /t/gui_test.py 2>&1 | grep -v "Gtk-WARNING\|dbind-WARNING"
echo "== bookmarks"; cat ~/.config/gtk-3.0/bookmarks 2>&1; ls $P/tmp/ | grep dav
