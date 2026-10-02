#!/bin/bash
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 procps psmisc mtools dosfstools >/dev/null 2>&1
gcc -shared -fPIC -O2 -o /tmp/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
P=/tmp/prefix; mkdir -p $P/bin $P/lib; cp /tmp/libdroiddesk_blk.so $P/lib/
for t in mkfs.fat mdir mcopy mdel mdeltree mmd mrd; do ln -s "$(command -v $t)" "$P/bin/$t"; done
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
truncate -s 1G /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 &
sleep 1
T() { echo "[$(date +%s)] $*"; }
timeout 120 $CLI format fat32 --label CONC --yes >/dev/null; echo "format rc=$?"
head -c 30000000 /dev/urandom > /tmp/c.bin
T "cp alone"; timeout 300 $CLI cp /tmp/c.bin usb:/alone >/dev/null 2>/tmp/alone.err; echo "rc=$?"; tail -2 /tmp/alone.err
T "cp + 3 ls + rm"
$CLI cp /tmp/c.bin usb:/a > /tmp/c1.log 2>&1 &
pids="$!"
sleep 0.5
for i in 1 2 3; do $CLI ls usb:/ > /dev/null 2>/tmp/ls$i.err & pids="$pids $!"; done
$CLI rm -r usb:/nothing-here >/dev/null 2>&1 & pids="$pids $!"
for i in $(seq 1 36); do sleep 5; alive=0; for p in $pids; do kill -0 $p 2>/dev/null && alive=1; done; [ $alive = 0 ] && break; done
[ $alive = 1 ] && { echo "FAIL: still running"; ps -eo pid,stat,wchan:30,args | grep -E "python3|mcopy|mdir" | grep -v grep; } || echo "ok: concurrent commands finished"
tail -1 /tmp/c1.log; grep -h "Čekám" /tmp/ls*.err | head -1
T "kill -9 client"
$CLI cp /tmp/c.bin usb:/b > /dev/null 2>&1 &
sleep 2; pkill -9 -f "droiddesk-usb.py cp /tmp/c.bin usb:/b"; sleep 2
pgrep -a mcopy && echo "FAIL: orphan" || echo "ok: no orphan"
$CLI ls usb:/ >/dev/null 2>&1 & p=$!; for i in $(seq 1 12); do sleep 5; kill -0 $p 2>/dev/null || break; done
kill -0 $p 2>/dev/null && echo "FAIL: ls after kill hangs" || echo "ok: ls after kill"
T "SIGTERM client"
$CLI cp /tmp/c.bin usb:/d > /dev/null 2>&1 & p=$!; sleep 2; kill -TERM $p
for i in $(seq 1 6); do sleep 1; kill -0 $p 2>/dev/null || break; done
kill -0 $p 2>/dev/null && echo "FAIL: client ignores SIGTERM" || echo "ok: client stopped"
sleep 1; pgrep -a mcopy && echo "FAIL: tool left" || echo "ok: tool stopped"
T "debug mode"
DROIDDESK_BLK_DEBUG=1 $CLI cp /tmp/c.bin usb:/dbg > /dev/null 2>/tmp/dbg.err & p=$!
for i in $(seq 1 24); do sleep 5; kill -0 $p 2>/dev/null || break; done
kill -0 $p 2>/dev/null && echo "FAIL: debug cp hangs" || { wait $p; echo "ok: debug cp rc=$?, $(wc -l < /tmp/dbg.err) debug lines"; }
T "dup/fork/exit"
gcc -o /tmp/fx /t/fx.c && DROIDDESK_BLK_DEVICE=/dev/bus/usb/001/002 DROIDDESK_BLK_OFFSET=1048576 LD_PRELOAD=$P/lib/libdroiddesk_blk.so /tmp/fx; echo "fx rc=$?"
echo "byte after exit without close: $(dd if=/tmp/disk.img bs=1 skip=1049088 count=1 status=none) (want Z)"
dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none; fsck.fat -n /tmp/p.img | tail -2
echo "== bridge"; grep -a DEADLOCK /tmp/bridge.log
