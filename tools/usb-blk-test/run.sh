#!/bin/bash
# Runs inside debian:trixie: droiddesk-usb format/files against an image through the shim.
set -u
export DEBIAN_FRONTEND=noninteractive
if ! command -v mkfs.exfat >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 procps mtools dosfstools exfatprogs e2fsprogs fdisk >/dev/null || exit 1
fi
cd /t
gcc -shared -fPIC -O2 -Wall -o /tmp/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl 2>&1 | grep -v "^$" | head -30
P=/tmp/prefix
rm -rf "$P"; mkdir -p "$P/bin" "$P/lib"
cp /tmp/libdroiddesk_blk.so "$P/lib/"
for t in mkfs.fat mkfs.exfat mkfs.ext4 debugfs mdir mcopy mdel mdeltree mmd mrd; do ln -s "$(command -v $t)" "$P/bin/$t"; done
ln -s "$(command -v python3)" "$P/bin/python3"
E=/src/cpp/exfat
gcc -shared -fPIC -O2 -w -I$E/libexfat -DPACKAGE='"droiddesk-exfat"' -DVERSION='"1.4.0"' -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE     -o "$P/lib/libdroiddesk_exfat.so" $E/exfat_tool.c $E/libexfat/*.c || echo "FAIL: exfat build"
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
BS=${BS:-512}
rm -f /tmp/disk.img; truncate -s ${SIZE:-1G} /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img $BS > /tmp/bridge.log 2>&1 &
sleep 1
fail=0
check() { if [ $1 -ne 0 ]; then echo "FAIL: $2"; fail=1; else echo "ok: $2"; fi; }
mkdir -p /tmp/up/sub; echo hello > /tmp/up/a.txt; head -c 3000000 /dev/urandom > /tmp/up/sub/big.bin; echo "čeština" > "/tmp/up/Příliš žluťoučký.txt"

for fs in fat32 ext4 exfat; do
    echo "=== $fs"
    $CLI format $fs --label TEST$fs --yes; check $? "format $fs"
    $CLI info
    sfdisk -d /tmp/disk.img 2>/dev/null | tail -2
    case $fs in
        fat32) fsck.fat -n /tmp/disk.img@@1048576 >/dev/null 2>&1; dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none; fsck.fat -n /tmp/p.img; check $? "fsck.fat after format" ;;
        ext4) dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none; e2fsck -fn /tmp/p.img >/dev/null; check $? "e2fsck after format" ;;
        exfat) dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none; fsck.exfat -n /tmp/p.img >/dev/null; check $? "fsck.exfat after format" ;;
    esac
    $CLI mkdir usb:/docs/deep; check $? "mkdir $fs"
    $CLI cp /tmp/up/a.txt /tmp/up/sub "/tmp/up/Příliš žluťoučký.txt" usb:/docs; check $? "cp up $fs"
    $CLI cp /tmp/up/a.txt usb:/docs; check $? "cp up again (overwrite) $fs"
    $CLI ls usb:/docs; check $? "ls $fs"
    $CLI ls --raw usb:/docs/sub
    rm -rf /tmp/down; mkdir /tmp/down
    $CLI cp usb:/docs /tmp/down; check $? "cp down dir $fs"
    $CLI cp usb:/docs/sub/big.bin /tmp/down/big2.bin; check $? "cp down file $fs"
    cmp /tmp/up/sub/big.bin /tmp/down/big2.bin; check $? "big.bin identical $fs"
    find /tmp/down -type f | sort
    diff -r /tmp/up /tmp/down/docs >/dev/null 2>&1 || find /tmp/down
    $CLI rm usb:/docs/a.txt; check $? "rm file $fs"
    $CLI rm -r usb:/docs/sub; check $? "rm -r $fs"
    $CLI ls usb:/docs
    dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none
    case $fs in
        fat32) fsck.fat -n /tmp/p.img; check $? "fsck.fat after files" ;;
        ext4) e2fsck -fn /tmp/p.img; check $? "e2fsck after files" ;;
        exfat) fsck.exfat -n /tmp/p.img; check $? "fsck.exfat after files" ;;
    esac
done
echo "=== bridge"; grep -ac session /tmp/bridge.log
[ $fail = 0 ] && echo ALL_OK || echo SOME_FAILED

# Throughput in bridge requests: 20 MB up and down on FAT32 with 4 KiB clusters (2 GB stick).
if [ -n "${PERF:-}" ]; then
    : > /tmp/bridge.log
    $CLI format fat32 --label PERF --yes >/dev/null
    head -c 20000000 /dev/urandom > /tmp/perf.bin
    : > /tmp/bridge.log
    $CLI cp /tmp/perf.bin usb:/ >/dev/null; echo "perf up:"; grep -a session /tmp/bridge.log | tail -3
    : > /tmp/bridge.log
    $CLI cp usb:/perf.bin /tmp/perf.back >/dev/null; echo "perf down:"; grep -a session /tmp/bridge.log | tail -3
    cmp /tmp/perf.bin /tmp/perf.back && echo "perf identical"
    : > /tmp/bridge.log
    $CLI format ext4 --yes >/dev/null; $CLI cp /tmp/perf.bin usb:/ >/dev/null; echo "perf ext4 up:"; grep -a session /tmp/bridge.log | tail -2
    : > /tmp/bridge.log
    $CLI cp usb:/perf.bin /tmp/perf.back2 >/dev/null; echo "perf ext4 down:"; grep -a session /tmp/bridge.log | tail -2
    cmp /tmp/perf.bin /tmp/perf.back2 && echo "perf ext4 identical"
fi

# Two commands on one disk, and a killed client (CONC=1).
if [ -n "${CONC:-}" ]; then
    $CLI format fat32 --label CONC --yes >/dev/null
    head -c 30000000 /dev/urandom > /tmp/c.bin
    ( $CLI cp /tmp/c.bin usb:/a > /tmp/c1.log 2>&1; echo "cp exit $?" >> /tmp/c1.log ) &
    sleep 0.5
    for i in 1 2 3; do $CLI ls usb:/ > /dev/null 2>/tmp/ls.err & done
    $CLI rm -r usb:/nothing-here >/dev/null 2>&1 &
    wait
    cat /tmp/c1.log | tail -1; grep -a "Čekám" /tmp/ls.err | head -1
    $CLI ls usb:/a; check $? "ls after concurrent commands"
    dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none; fsck.fat -n /tmp/p.img >/dev/null; check $? "fsck after concurrent commands"
    $CLI cp /tmp/c.bin usb:/b > /dev/null 2>&1 &
    sleep 1; pkill -9 -f "droiddesk-usb.py cp /tmp/c.bin usb:/b"; sleep 1
    pgrep -a mcopy && { echo "FAIL: orphan mcopy"; fail=1; } || echo "ok: no orphan after kill -9"
    timeout 30 $CLI ls usb:/ >/dev/null; check $? "ls after killed client"
    $CLI cp /tmp/c.bin usb:/d > /dev/null 2>&1 & sleep 1; kill -TERM $!; wait $! 2>/dev/null
    sleep 1; pgrep -a mcopy && { echo "FAIL: mcopy after SIGTERM"; fail=1; } || echo "ok: SIGTERM stops the tool"
    timeout 30 $CLI ls usb:/ >/dev/null; check $? "ls after SIGTERM"
    [ $fail = 0 ] && echo CONC_OK || echo CONC_FAILED
fi
