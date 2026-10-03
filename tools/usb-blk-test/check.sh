#!/bin/bash
# droiddesk-usb check: stable disk, then a fake-capacity disk (FAKE_WRAP).
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq python3 gcc libc6-dev mtools dosfstools >/dev/null 2>&1
P=/tmp/prefix; mkdir -p $P/bin $P/lib
gcc -shared -fPIC -O2 -o $P/lib/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
for t in mkfs.fat mdir mcopy mdel mdeltree mmd mrd; do ln -sf "$(command -v $t)" "$P/bin/$t"; done
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
truncate -s 64M /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 & b=$!; sleep 1
$CLI check; echo "read check rc=$? (want 0)"
$CLI check --write --yes; echo "write check rc=$? (want 0)"
kill $b; sleep 1
FAKE_WRAP=16777216 python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 & sleep 1
$CLI check --write --yes; echo "fake check rc=$? (want 1, first bad at 16 MB)"
