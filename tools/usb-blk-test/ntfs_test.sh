#!/bin/bash
# Runs inside debian:trixie: libdroiddesk_ntfs.so (droiddesk-ntfs on libntfs-3g) through the shim
# against the fake bridge, mkntfs through the shim the way droiddesk-usb formats, and Debian's
# ntfs-3g tools (ntfsfix, ntfsls, ntfscat) on the partition image as the independent check.
#
#   docker run --rm -v "$PWD/tools/usb-blk-test:/t" -v "$PWD/app/android/app/src/main:/src:ro" \
#       debian:trixie bash /t/ntfs_test.sh          # ends with NTFS_ALL_OK
set -u
export DEBIAN_FRONTEND=noninteractive
if ! command -v mkntfs >/dev/null || ! command -v gcc >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 ntfs-3g fdisk >/dev/null 2>&1 || exit 1
fi
fail=0
check() { if [ "$1" -ne 0 ]; then echo "FAIL: $2"; fail=1; else echo "ok: $2"; fi; }

gcc -shared -fPIC -O2 -Wall -o /tmp/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl
N=/src/cpp/ntfs
gcc -fsyntax-only -Wall -Wextra -DHAVE_CONFIG_H -I$N -I$N/include/ntfs-3g $N/ntfs_tool.c 2>/tmp/warn.txt
[ -s /tmp/warn.txt ] && cat /tmp/warn.txt
test ! -s /tmp/warn.txt; check $? "ntfs_tool.c without warnings (-Wall -Wextra)"
gcc -shared -fPIC -O2 -w -DHAVE_CONFIG_H -I$N -I$N/include/ntfs-3g -o /tmp/libdroiddesk_ntfs.so \
    $N/ntfs_tool.c $N/libntfs-3g/*.c
check $? "build libdroiddesk_ntfs.so"

# Disk: MBR with one NTFS partition (type 0x07) from 1 MiB to the end.
SIZE=${SIZE:-1G}
rm -f /tmp/disk.img; truncate -s "$SIZE" /tmp/disk.img
printf 'label: dos\nstart=2048, type=7\n' | sfdisk -q /tmp/disk.img
check $? "MBR with one partition at 1 MiB"
START=1048576
PSIZE=$(( ($(stat -c %s /tmp/disk.img) / 512 - 2048) * 512 ))
python3 /t/fake_bridge.py /tmp/disk.img 512 > /tmp/bridge.log 2>&1 &
sleep 1

blk() { LD_PRELOAD=/tmp/libdroiddesk_blk.so DROIDDESK_BLK_DEVICE=/dev/bus/usb/001/002 DROIDDESK_BLK_LUN=- \
        DROIDDESK_BLK_OFFSET=$START DROIDDESK_BLK_SIZE=$PSIZE "$@"; }
RUNNER='import ctypes, sys
tool = ctypes.CDLL(sys.argv[1])
args = [a.encode() for a in ["droiddesk-ntfs"] + sys.argv[2:]]
sys.exit(tool.droiddesk_ntfs_main(len(args), (ctypes.c_char_p * len(args))(*args)))'
ntfs() { blk python3 -c "$RUNNER" /tmp/libdroiddesk_ntfs.so "$@"; }
part() { dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none; }

echo "=== mkntfs through the shim"
MKNTFS="mkntfs -Q -F -L DROIDDESK -s 512 -p 2048 -H 255 -S 63 /dev/droiddesk-blk"
echo "$MKNTFS"
blk $MKNTFS > /tmp/mkntfs.log 2>&1; rc=$?
tail -3 /tmp/mkntfs.log
check $rc "mkntfs"
python3 - "$START" <<'EOF'
import struct, sys
with open("/tmp/disk.img", "rb") as f:
    f.seek(int(sys.argv[1])); b = f.read(512)
oem, bps, spc = b[3:11], struct.unpack_from("<H", b, 11)[0], b[13]
spt, heads, hidden = struct.unpack_from("<HHI", b, 24)
total = struct.unpack_from("<Q", b, 40)[0]
print(f"boot sector: {oem!r} bytes/sector {bps} sectors/cluster {spc} spt {spt} heads {heads} hidden {hidden} total {total}")
sys.exit(0 if oem == b"NTFS    " and hidden == 2048 and heads == 255 and spt == 63 and b[510:512] == b"\x55\xaa" else 1)
EOF
check $? "boot sector: hidden sectors 2048, 255 heads, 63 sectors/track"
part; ntfsfix -n /tmp/p.img > /tmp/fix.log 2>&1; rc=$?; grep -iE "error|corrupt|fail" /tmp/fix.log
check $rc "ntfsfix -n after format"

echo "=== files"
mkdir -p /tmp/up; echo hello > /tmp/up/a.txt; echo "hello again, longer" > /tmp/up/a2.txt
head -c 30000000 /dev/urandom > /tmp/up/big.bin; head -c 5000 /dev/urandom > /tmp/up/small.bin
echo "čeština ěščřžýáíé" > "/tmp/up/Příliš žluťoučký kůň.txt"; : > /tmp/up/empty
CZ="Příliš žluťoučký kůň.txt"
printf 'mkdir\t/docs\nmkdir\t/docs/deep\nmkdir\t/docs/deep/deeper\nmkdir\t/docs\nput\t/tmp/up/a.txt\t/docs/a.txt\nput\t/tmp/up/big.bin\t/docs/deep/big.bin\nput\t%s\t/docs/%s\nput\t/tmp/up/empty\t/docs/empty\nput\t/tmp/up/small.bin\t/docs/deep/deeper/small.bin\n' \
    "/tmp/up/$CZ" "$CZ" | ntfs /dev/droiddesk-blk > /tmp/o1.txt; rc=$?; cat /tmp/o1.txt
check $rc "mkdir nested + put (30 MB, Czech name, empty file)"
printf 'mkdir\t/DOCS\nput\t/tmp/up/a2.txt\t/docs/A.TXT\nput\t/tmp/up/small.bin\t/docs/deep/big.bin\nput\t/tmp/up/big.bin\t/docs/deep/big.bin\n' | ntfs /dev/droiddesk-blk
check $? "put again (overwrite, shrink, grow; A.TXT replaces a.txt, mkdir /DOCS finds docs)"
printf 'ls\t/docs\n' | ntfs --ro /dev/droiddesk-blk > /tmp/ls.txt; rc=$?; cat /tmp/ls.txt
check $rc "ls"
printf 'L\tf\t20\ta.txt\nL\td\t0\tdeep\nL\tf\t0\tempty\nL\tf\t%s\t%s\n' "$(stat -c %s "/tmp/up/$CZ")" "$CZ" | sort > /tmp/ls.want
sort /tmp/ls.txt | cmp -s - /tmp/ls.want; check $? "ls output exact"
printf 'tree\t/\n' | ntfs --ro /dev/droiddesk-blk > /tmp/tree.txt; rc=$?; cat /tmp/tree.txt
check $rc "tree"
grep -q '	\$' /tmp/tree.txt; test $? -ne 0; check $? "tree hides \$ metafiles"
grep -q "^T	f	30000000	docs/deep/big.bin$" /tmp/tree.txt; check $? "tree lists docs/deep/big.bin with its size"
printf 'stat\t/docs/deep\nstat\t/docs/deep/big.bin\nstat\t/\n' | ntfs --ro /dev/droiddesk-blk > /tmp/st.txt; rc=$?
printf 'S\td\t0\nS\tf\t30000000\nS\td\t0\n' | cmp -s - /tmp/st.txt; check $(( rc + $? )) "stat"
rm -rf /tmp/down; mkdir /tmp/down
printf 'get\t/docs/deep/big.bin\t/tmp/down/big.bin\nget\t/docs/a.txt\t/tmp/down/a.txt\nget\t/docs/%s\t/tmp/down/cz.txt\nget\t/docs/empty\t/tmp/down/empty\nget\t/docs/deep/deeper/small.bin\t/tmp/down/small.bin\n' "$CZ" \
    | ntfs --ro /dev/droiddesk-blk; check $? "get (read-only mount)"
cmp /tmp/up/big.bin /tmp/down/big.bin; check $? "big.bin identical"
cmp /tmp/up/a2.txt /tmp/down/a.txt; check $? "a.txt has the overwritten content"
cmp "/tmp/up/$CZ" /tmp/down/cz.txt; check $? "Czech name file identical"
cmp /tmp/up/empty /tmp/down/empty && cmp /tmp/up/small.bin /tmp/down/small.bin; check $? "empty + small identical"
[ "$(stat -c %Y /tmp/down/big.bin)" = "$(stat -c %Y /tmp/up/big.bin)" ]; check $? "mtime kept both ways"

echo "=== errors"
printf 'put\t/tmp/up/a.txt\t/docs/x.txt\n' | ntfs --ro /dev/droiddesk-blk > /tmp/e.txt; rc=$?; cat /tmp/e.txt
[ $rc = 1 ] && grep -q "^E	put	" /tmp/e.txt; check $? "put refused on --ro"
printf 'stat\t/nope\nrmdir\t/docs/deep\nrm\t/docs/deep\nrmdir\t/docs/a.txt\nput\t/tmp/up/a.txt\t/docs/bad:name\nmkdir\t/docs/a.txt\nrm\t/$MFT\nls\t/docs\n' | ntfs /dev/droiddesk-blk > /tmp/e.txt; rc=$?; cat /tmp/e.txt
[ $rc = 1 ] && [ "$(grep -c '^E	' /tmp/e.txt)" = 7 ] && grep -q "^L	" /tmp/e.txt; check $? "7 errors reported, batch goes on, exit 1"

echo "=== Windows view (Debian ntfs-3g tools on the partition image)"
part; ntfsfix -n /tmp/p.img > /tmp/fix.log 2>&1; rc=$?; cat /tmp/fix.log | tail -4
check $rc "ntfsfix -n after files"
ntfsls -a -R /tmp/p.img > /tmp/ntfsls.txt 2>&1; rc=$?
grep -vE "^\$|^\.\.?$" /tmp/ntfsls.txt | head -30
grep -q "$CZ" /tmp/ntfsls.txt && grep -q "small.bin" /tmp/ntfsls.txt; check $(( rc + $? )) "ntfsls -R sees the tree"
ntfscat /tmp/p.img /docs/deep/big.bin | cmp - /tmp/up/big.bin; check $? "ntfscat big.bin identical"
ntfscat /tmp/p.img "/docs/$CZ" | cmp - "/tmp/up/$CZ"; check $? "ntfscat Czech name identical"
ntfsinfo -F /docs/deep/big.bin /tmp/p.img 2>/dev/null | grep -E "Namespace|Data size|Allocated size|Runlist|Security" | head -8

echo "=== delete"
printf 'rm\t/docs/a.txt\nrm\t/docs/deep/deeper/small.bin\nrmdir\t/docs/deep/deeper\nrm\t/docs/deep/big.bin\nrmdir\t/docs/deep\nls\t/docs\n' | ntfs /dev/droiddesk-blk > /tmp/d.txt; rc=$?; cat /tmp/d.txt
check $rc "rm + rmdir"
[ "$(grep -c '^L	' /tmp/d.txt)" = 2 ]; check $? "two entries left"
part; ntfsfix -n /tmp/p.img > /tmp/fix.log 2>&1; check $? "ntfsfix -n after delete"
ntfsls -R /tmp/p.img 2>&1 | grep -qE "big.bin|deeper"; test $? -ne 0; check $? "ntfsls: deleted entries gone"

echo "=== many files (index grows past one block)"
for i in $(seq 1 300); do printf 'put\t/tmp/up/a.txt\t/docs/many/file-%03d-with-a-long-name.txt\n' $i; done > /tmp/many.cmd
{ printf 'mkdir\t/docs/many\n'; cat /tmp/many.cmd; printf 'ls\t/docs/many\n'; } | ntfs /dev/droiddesk-blk > /tmp/m.txt; rc=$?
[ $rc = 0 ] && [ "$(grep -c '^L	' /tmp/m.txt)" = 300 ]; check $? "300 files in one folder"
part; ntfsfix -n /tmp/p.img > /tmp/fix.log 2>&1; check $? "ntfsfix -n after 300 files"
[ "$(ntfsls -a -p /docs/many /tmp/p.img | grep -c "^file-")" = 300 ]; check $? "ntfsls sees 300 files"

# Find a record of the MFT: data of $MFT is contiguous right after mkntfs.
setdirty() {
python3 - "$START" "$1" <<'EOF'
import struct, sys
start, on = int(sys.argv[1]), sys.argv[2] == "1"
f = open("/tmp/disk.img", "r+b"); f.seek(start); b = f.read(512)
bps = struct.unpack_from("<H", b, 11)[0]; cl = bps * b[13]
mft, mirr = struct.unpack_from("<QQ", b, 48); c = struct.unpack_from("<b", b, 64)[0]
rec = cl * c if c > 0 else 1 << -c
for lcn in (mft, mirr):
    pos = start + lcn * cl + 3 * rec          # record 3: $Volume
    f.seek(pos); r = bytearray(f.read(rec))
    a = struct.unpack_from("<H", r, 20)[0]
    while True:
        t, ln = struct.unpack_from("<II", r, a)
        if t == 0xFFFFFFFF: sys.exit("no VOLUME_INFORMATION")
        if t == 0x70:
            v = a + struct.unpack_from("<H", r, a + 20)[0]
            flags = struct.unpack_from("<H", r, v + 10)[0]
            flags = flags | 1 if on else flags & ~1
            struct.pack_into("<H", r, v + 10, flags)
            assert (v + 10) % bps < bps - 2  # not a fixup position
            break
        a += ln
    f.seek(pos); f.write(r)
EOF
}

echo "=== dirty volume (chkdsk flag)"
setdirty 1; check $? "dirty flag set"
printf 'mkdir\t/x\n' | ntfs /dev/droiddesk-blk > /tmp/x.txt; rc=$?; cat /tmp/x.txt
[ $rc = 1 ] && grep -q "^E	mount	.*chkdsk" /tmp/x.txt; check $? "write refused on a dirty volume"
printf 'ls\t/docs\n' | ntfs --ro /dev/droiddesk-blk >/dev/null; check $? "read-only works on a dirty volume"
setdirty 0

echo "=== hibernated Windows"
{ printf 'HIBR'; head -c 8188 /dev/zero; } > /tmp/hiberfil.sys
printf 'put\t/tmp/hiberfil.sys\t/hiberfil.sys\n' | ntfs /dev/droiddesk-blk; check $? "fake hiberfil.sys written"
printf 'mkdir\t/x\n' | ntfs /dev/droiddesk-blk > /tmp/x.txt; rc=$?; cat /tmp/x.txt
[ $rc = 1 ] && grep -q "^E	mount	NTFS je označený jako používaný (Windows hibernace/rychlé spuštění) — zápis odmítnut$" /tmp/x.txt
check $? "write refused on a hibernated volume"
printf 'ls\t/docs\n' | ntfs --ro /dev/droiddesk-blk > /tmp/x.txt; check $? "read-only works on a hibernated volume"
part; ntfsfix -n /tmp/p.img > /tmp/fix.log 2>&1; check $? "ntfsfix -n at the end"

echo "=== bridge"; grep -ac session /tmp/bridge.log
[ $fail = 0 ] && echo NTFS_ALL_OK || echo NTFS_SOME_FAILED
