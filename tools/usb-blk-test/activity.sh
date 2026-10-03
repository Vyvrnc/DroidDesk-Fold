#!/bin/bash
# Status files for the USB disky window: progress while running, result at the end.
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq python3 gcc libc6-dev mtools dosfstools >/dev/null 2>&1
P=/tmp/prefix; mkdir -p $P/bin $P/lib $P/tmp
gcc -shared -fPIC -O2 -o $P/lib/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
for t in mkfs.fat mdir mcopy mdel mdeltree mmd mrd; do ln -sf "$(command -v $t)" "$P/bin/$t"; done
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
truncate -s 64M /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 & sleep 1
$CLI check >/dev/null 2>&1 & c=$!
for i in 1 2 3 4 5 6; do sleep 2; cat $P/tmp/*.status 2>/dev/null; echo; done
wait $c; echo "final:"; cat $P/tmp/*.status; echo
$CLI format fat32 --yes >/dev/null; cat $P/tmp/*.status; echo
$CLI cp /nonexistent usb:/ 2>/dev/null; cat $P/tmp/*.status; echo
