#!/bin/bash
# Repeated recursive upload/download with random file sizes through droiddesk-usb on one
# filesystem (FS=exfat by default), comparing every round. ROUNDS=n, SIZE=disk size.
set -u
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 procps mtools dosfstools exfatprogs e2fsprogs ntfs-3g >/dev/null 2>&1
P=/tmp/prefix; mkdir -p $P/bin $P/lib
gcc -shared -fPIC -O2 -o $P/lib/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
E=/src/cpp/exfat
gcc -shared -fPIC -O2 -w -I$E/libexfat -DPACKAGE='"droiddesk-exfat"' -DVERSION='"1.4.0"' -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE \
    -o $P/lib/libdroiddesk_exfat.so $E/exfat_tool.c $E/libexfat/*.c
N=/src/cpp/ntfs
gcc -shared -fPIC -O2 -w -I$N -I$N/include/ntfs-3g -DHAVE_CONFIG_H -o $P/lib/libdroiddesk_ntfs.so $N/ntfs_tool.c $N/libntfs-3g/*.c
for t in mkfs.fat mkfs.exfat mkfs.ext4 mkntfs debugfs mdir mcopy mdel mdeltree mmd mrd python3; do ln -sf "$(command -v $t)" "$P/bin/$t"; done
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
FS=${FS:-exfat}
truncate -s ${SIZE:-2G} /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 &
sleep 1
$CLI format $FS --label STRESS --yes >/dev/null || { echo "FAIL: format"; exit 1; }
fail=0
for round in $(seq 1 ${ROUNDS:-8}); do
    rm -rf /tmp/up /tmp/back; mkdir -p /tmp/up/d/e /tmp/back
    for i in $(seq 1 6); do
        # sizes around cluster, window and 1 MiB request boundaries
        n=$(( (RANDOM * 32768 + RANDOM) % 9000000 + RANDOM % 7 ))
        head -c $n /dev/urandom > /tmp/up/d/f$i.bin
    done
    head -c $(( RANDOM % 70000 )) /dev/urandom > /tmp/up/d/e/small.bin
    head -c 31457280 /dev/urandom > /tmp/up/d/big.bin
    $CLI cp /tmp/up/d usb:/r$round >/dev/null 2>/tmp/err.txt || { echo "FAIL: upload round $round: $(tail -1 /tmp/err.txt)"; fail=1; }
    $CLI cp usb:/r$round /tmp/back >/dev/null 2>/tmp/err.txt || { echo "FAIL: download round $round: $(tail -1 /tmp/err.txt)"; fail=1; }
    if diff -r /tmp/up/d /tmp/back/r$round >/tmp/diff.txt; then echo "ok: round $round identical"
    else echo "FAIL: round $round differs: $(head -3 /tmp/diff.txt | tr '\n' ' ')"; fail=1
        for f in /tmp/up/d/*.bin; do cmp "$f" "/tmp/back/r$round/$(basename $f)" 2>&1 | head -1; done
    fi
    # every other round: delete the previous round (fragmentation, reuse of freed clusters)
    [ $((round % 2)) = 0 ] && $CLI rm -r usb:/r$((round - 1)) >/dev/null
done
dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none
case $FS in
    exfat) fsck.exfat -n /tmp/p.img | tail -2 ;;
    ntfs) ntfsfix -n /tmp/p.img | tail -1 ;;
    fat32) fsck.fat -n /tmp/p.img | tail -1 ;;
    ext4) e2fsck -fn /tmp/p.img | tail -1 ;;
esac
[ $fail = 0 ] && echo STRESS_OK || echo STRESS_FAILED
