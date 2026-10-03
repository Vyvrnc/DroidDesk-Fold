#!/bin/bash
# Runs inside debian:trixie: droiddesk-usb mv and droiddesk-usb-dav (WebDAV) on fat32, ext4,
# exfat and ntfs through the shim and the fake bridge; curl as the client, fsck at the end of
# each filesystem. GVFS=1 also mounts it with gio (the gvfs dav backend Thunar uses).
#
#   docker run --rm -v "$PWD/tools/usb-blk-test:/t" -v "$PWD/app/android/app/src/main:/src:ro" \
#       debian:trixie bash /t/dav.sh            # ends with DAV_ALL_OK
set -u
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
if ! command -v mkntfs >/dev/null || ! command -v curl >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 procps mtools dosfstools \
        exfatprogs e2fsprogs ntfs-3g fdisk curl >/dev/null 2>&1 || exit 1
fi
gcc -shared -fPIC -O2 -Wall -o /tmp/libdroiddesk_blk.so /src/cpp/blk_shim.c -ldl 2>&1 | head -30
P=/tmp/prefix
rm -rf "$P"; mkdir -p "$P/bin" "$P/lib"
cp /tmp/libdroiddesk_blk.so "$P/lib/"
for t in mkfs.fat mkfs.exfat mkfs.ext4 mkntfs debugfs mdir mcopy mdel mdeltree mmd mrd mmove mshowfat; do ln -s "$(command -v $t)" "$P/bin/$t"; done
ln -s "$(command -v python3)" "$P/bin/python3"
E=/src/cpp/exfat
gcc -shared -fPIC -O2 -w -I$E/libexfat -DPACKAGE='"droiddesk-exfat"' -DVERSION='"1.4.0"' -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE \
    -o "$P/lib/libdroiddesk_exfat.so" $E/exfat_tool.c $E/libexfat/*.c || echo "FAIL: exfat build"
N=/src/cpp/ntfs
gcc -fsyntax-only -Wall -Wextra -DHAVE_CONFIG_H -I$N -I$N/include/ntfs-3g $N/ntfs_tool.c 2>&1 | head -20
gcc -shared -fPIC -O2 -w -I$N -I$N/include/ntfs-3g -DHAVE_CONFIG_H \
    -o "$P/lib/libdroiddesk_ntfs.so" $N/ntfs_tool.c $N/libntfs-3g/*.c || echo "FAIL: ntfs build"
export DROIDDESK_PREFIX=$P TMPDIR=/tmp
CLI="python3 /src/assets/droiddesk/droiddesk-usb.py"
DAV="python3 /src/assets/droiddesk/droiddesk-usb-dav.py"
DEV=/dev/bus/usb/001/002
STATE=$P/tmp/droiddesk-usb-dav_dev_bus_usb_001_002.json
rm -f /tmp/disk.img; truncate -s ${SIZE:-1G} /tmp/disk.img
python3 /t/fake_bridge.py /tmp/disk.img ${BS:-512} > /tmp/bridge.log 2>&1 &
BRIDGE=$!
sleep 1
fail=0
check() { if [ "$1" -ne 0 ]; then echo "FAIL: $2"; fail=1; else echo "ok: $2"; fi; }
expect() { [ "$1" = "$2" ]; check $? "$3 (HTTP $1, expected $2)"; }
part() { dd if=/tmp/disk.img of=/tmp/p.img bs=1M skip=1 status=none; }
fsck_part() {
    part
    case $1 in
        fat32) fsck.fat -n /tmp/p.img > /tmp/fsck.log 2>&1 ;;
        ext4) e2fsck -fn /tmp/p.img > /tmp/fsck.log 2>&1 ;;
        exfat) fsck.exfat -n /tmp/p.img > /tmp/fsck.log 2>&1 ;;
        ntfs) ntfsfix -n /tmp/p.img > /tmp/fsck.log 2>&1 ;;
    esac
    rc=$?; [ $rc = 0 ] || tail -15 /tmp/fsck.log
    check $rc "fsck $1 $2"
}
q() { python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$1"; }
code() {  # HTTP status; the body goes to /tmp/body unless the call starts with -o FILE
    if [ "$1" = -o ]; then out=$2; shift 2; else out=/tmp/body; fi
    curl -s -o "$out" -w '%{http_code}' "$@"
}

mkdir -p /tmp/up; echo hello > /tmp/up/a.txt; echo "druhý obsah" > /tmp/up/b.txt
head -c 30000000 /dev/urandom > /tmp/up/big.bin
CZ="Příliš žluťoučký kůň.txt"; echo "čeština ěščřžýáíé" > "/tmp/up/$CZ"

for fs in ${FSLIST:-fat32 ext4 exfat ntfs}; do
    echo "=== $fs: droiddesk-usb mv"
    $CLI format $fs --label T$fs --yes >/dev/null; check $? "format $fs"
    $CLI mkdir usb:/m/sub usb:/m/other >/dev/null && $CLI cp /tmp/up/a.txt usb:/m/sub >/dev/null && \
        $CLI cp /tmp/up/a.txt usb:/m/one.txt >/dev/null; check $? "setup $fs"
    $CLI mv usb:/m/one.txt usb:/m/two.txt; check $? "mv rename file $fs"
    $CLI mv usb:/m/two.txt usb:/m/sub; check $? "mv file into folder $fs"
    $CLI mv usb:/m/sub usb:/n; check $? "mv folder to another parent $fs"
    $CLI ls --raw usb:/n | sort | tr '\n' ' ' > /tmp/ls.txt; cat /tmp/ls.txt; echo
    [ "$(cat /tmp/ls.txt)" = "f	6	a.txt f	6	two.txt " ]; check $? "moved folder has its files $fs"
    $CLI ls --raw usb:/m | grep -q sub; test $? -ne 0; check $? "old folder gone $fs"
    $CLI mv usb:/m/other usb:/n/inner; check $? "mv folder into a deeper folder $fs"
    $CLI cp /tmp/up/b.txt usb:/n/b.txt >/dev/null
    $CLI mv usb:/n/b.txt usb:/n/a.txt; check $? "mv replaces an existing file $fs"
    rm -f /tmp/a.back; $CLI cp usb:/n/a.txt /tmp/a.back >/dev/null; cmp /tmp/up/b.txt /tmp/a.back; check $? "replaced content $fs"
    $CLI mv usb:/n/a.txt usb:/n/A.TXT; check $? "mv case only $fs"
    $CLI ls --raw usb:/n | grep -q "	A.TXT$"; check $? "case changed $fs"
    $CLI mv usb:/n/A.TXT "usb:/n/$CZ"; check $? "mv to a Czech name $fs"
    $CLI ls --raw usb:/n | grep -q "	$CZ$"; check $? "Czech name listed $fs"
    $CLI mv usb:/n usb:/n/inner/x 2>/dev/null; test $? -ne 0; check $? "mv folder into itself refused $fs"
    $CLI mv usb:/n/inner usb:/n/two.txt 2>/dev/null; test $? -ne 0; check $? "mv folder over a file refused $fs"
    $CLI ls --raw usb:/n | sort | tr '\n' ' '; echo
    fsck_part $fs "after mv"

    if [ $fs = ext4 ]; then
        echo "=== ext4: folder with an htree index (copy fallback)"
        $CLI mkdir usb:/big usb:/dest >/dev/null
        mkdir -p /tmp/many; for i in $(seq 1 150); do echo $i > /tmp/many/file-number-$i-with-a-rather-long-name.txt; done
        $CLI cp /tmp/many/* usb:/big >/dev/null
        part; e2fsck -fyD /tmp/p.img >/dev/null 2>&1
        dd if=/tmp/p.img of=/tmp/disk.img bs=1M seek=1 conv=notrunc status=none
        debugfs -R "stat /big" /tmp/p.img 2>/dev/null | grep -o "Flags: 0x[0-9a-f]*"
        $CLI mv usb:/big usb:/dest/big; check $? "mv indexed folder $fs"
        [ "$($CLI ls --raw usb:/dest/big | wc -l)" = 150 ]; check $? "150 files after the move $fs"
        fsck_part $fs "after the indexed folder move"
    fi

    echo "=== $fs: WebDAV"
    rm -f $STATE
    $DAV $DEV > /tmp/dav.out 2> /tmp/dav.log &
    DAVPID=$!
    for i in $(seq 1 100); do [ -s $STATE ] && break; sleep 0.1; done
    [ -s $STATE ]; check $? "server started, state file $fs"
    cat $STATE; echo
    PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' $STATE)
    U=http://127.0.0.1:$PORT
    [ "$($DAV $DEV)" = "dav://localhost:$PORT/" ]; check $? "second start prints the running server's URL $fs"

    curl -s -D /tmp/h -o /dev/null -X OPTIONS $U/; grep -qi "^DAV: 1, 2" /tmp/h; check $? "OPTIONS DAV: 1, 2 $fs"
    expect "$(code -H 'Host: evil.example' -X PROPFIND $U/)" 403 "foreign Host refused"
    expect "$(code -H 'Origin: http://evil.example' $U/)" 403 "Origin refused"
    expect "$(code -X PROPFIND -H 'Depth: 0' $U/)" 207 "PROPFIND depth 0 /"
    expect "$(code -X MKCOL $U/a)" 201 "MKCOL /a"
    expect "$(code -X MKCOL $U/a)" 405 "MKCOL existing"
    expect "$(code -X MKCOL $U/a/b/c)" 409 "MKCOL without parent"
    expect "$(code -X MKCOL $U/a/b)" 201 "MKCOL /a/b"
    expect "$(code -X MKCOL $U/a/b/c)" 201 "MKCOL /a/b/c (nested)"
    expect "$(code -T /tmp/up/big.bin $U/a/b/c/big.bin)" 201 "PUT 30 MB"
    expect "$(code -T /tmp/up/a.txt $U/a/b/c/big.bin)" 204 "PUT replace (smaller)"
    expect "$(code -T /tmp/up/big.bin $U/a/b/c/big.bin)" 204 "PUT replace (30 MB again)"
    expect "$(code -T /tmp/up/a.txt $U/nope/x.txt)" 409 "PUT without parent"
    expect "$(code -X PROPFIND -H 'Depth: 1' $U/a/b/c/)" 207 "PROPFIND depth 1"
    grep -q "<D:href>/a/b/c/big.bin</D:href>" /tmp/body && grep -q "<D:getcontentlength>30000000<" /tmp/body
    check $? "PROPFIND lists big.bin with its size $fs"
    python3 -c 'import sys, xml.dom.minidom; xml.dom.minidom.parse(sys.argv[1])' /tmp/body; check $? "PROPFIND is well-formed XML"
    expect "$(code -o /tmp/g.bin $U/a/b/c/big.bin)" 200 "GET 30 MB"
    cmp /tmp/up/big.bin /tmp/g.bin; check $? "GET content identical $fs"
    expect "$(code -o /tmp/r.bin -H 'Range: bytes=1000-1999' $U/a/b/c/big.bin)" 206 "GET Range"
    dd if=/tmp/up/big.bin bs=1000 skip=1 count=1 status=none | cmp - /tmp/r.bin; check $? "Range content $fs"
    expect "$(code -o /tmp/r.bin -H 'Range: bytes=29999000-' $U/a/b/c/big.bin)" 206 "GET open Range"
    tail -c 1000 /tmp/up/big.bin | cmp - /tmp/r.bin; check $? "open Range content $fs"
    expect "$(code -o /tmp/r.bin -H 'Range: bytes=-100' $U/a/b/c/big.bin)" 206 "GET suffix Range"
    tail -c 100 /tmp/up/big.bin | cmp - /tmp/r.bin; check $? "suffix Range content $fs"
    expect "$(code -H 'Range: bytes=40000000-' $U/a/b/c/big.bin)" 416 "Range beyond the end"
    curl -s -I $U/a/b/c/big.bin | grep -qi "^Content-Length: 30000000"; check $? "HEAD length $fs"
    CZU=$(q "$CZ")
    expect "$(code -T "/tmp/up/$CZ" "$U/a/$CZU")" 201 "PUT Czech name with spaces"
    expect "$(code -X PROPFIND -H 'Depth: 1' $U/a/)" 207 "PROPFIND /a/"
    grep -q "<D:href>/a/$CZU</D:href>" /tmp/body && grep -q "<D:displayname>$CZ</D:displayname>" /tmp/body
    check $? "Czech name: href encoded, displayname UTF-8 $fs"
    expect "$(code -o /tmp/cz.back "$U/a/$CZU")" 200 "GET Czech name"
    cmp "/tmp/up/$CZ" /tmp/cz.back; check $? "Czech content $fs"

    expect "$(code -X MOVE -H "Destination: $U/a/moved.bin" $U/a/b/c/big.bin)" 201 "MOVE file"
    expect "$(code -X PROPFIND -H 'Depth: 0' $U/a/b/c/big.bin)" 404 "MOVE source gone"
    expect "$(code -o /tmp/g.bin $U/a/moved.bin)" 200 "GET moved"
    cmp /tmp/up/big.bin /tmp/g.bin; check $? "moved content $fs"
    # How editors save: a temporary file renamed over the original.
    code -T /tmp/up/a.txt $U/a/doc.txt >/dev/null
    code -T /tmp/up/b.txt $U/a/.doc.txt.tmp >/dev/null
    expect "$(code -X MOVE -H "Destination: http://localhost:$PORT/a/doc.txt" $U/a/.doc.txt.tmp)" 204 "MOVE over an existing file"
    expect "$(code -o /tmp/d.back $U/a/doc.txt)" 200 "GET replaced"
    cmp /tmp/up/b.txt /tmp/d.back; check $? "replaced by MOVE: new content $fs"
    expect "$(code -X PROPFIND -H 'Depth: 0' $U/a/.doc.txt.tmp)" 404 "temporary file gone"
    code -T /tmp/up/a.txt $U/a/keep.txt >/dev/null
    expect "$(code -X MOVE -H 'Overwrite: F' -H "Destination: $U/a/keep.txt" $U/a/doc.txt)" 412 "MOVE Overwrite: F"
    expect "$(code -X MOVE -H "Destination: $U/d" $U/a/b)" 201 "MOVE folder to another parent"
    expect "$(code -X PROPFIND -H 'Depth: 1' $U/d/c/)" 207 "moved folder content"
    grep -q "<D:href>/d/c/</D:href>" /tmp/body; check $? "href of the moved folder $fs"
    expect "$(code -X MOVE -H "Destination: $U/e" $U/d)" 201 "MOVE folder rename"
    expect "$(code -X MOVE -H "Destination: $U/e/c/x" $U/e)" 403 "MOVE folder into itself"

    expect "$(code -X COPY -H "Destination: $U/e/c/copy.bin" $U/a/moved.bin)" 201 "COPY file"
    expect "$(code -o /tmp/g.bin $U/e/c/copy.bin)" 200 "GET copy"
    cmp /tmp/up/big.bin /tmp/g.bin; check $? "copy content $fs"
    expect "$(code -X COPY -H "Destination: $U/f" $U/e)" 201 "COPY folder"
    expect "$(code -o /tmp/g.bin $U/f/c/copy.bin)" 200 "GET from the copied folder"
    cmp /tmp/up/big.bin /tmp/g.bin; check $? "copied folder content $fs"
    expect "$(code -X COPY -H "Destination: $U/f" $U/e)" 204 "COPY folder over a folder"

    expect "$(code -X LOCK -H 'Content-Type: application/xml' --data '<?xml version="1.0"?><D:lockinfo xmlns:D="DAV:"><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype><D:owner>me</D:owner></D:lockinfo>' $U/a/keep.txt)" 200 "LOCK"
    grep -q "opaquelocktoken:" /tmp/body; check $? "LOCK token $fs"
    expect "$(code -X UNLOCK -H 'Lock-Token: <opaquelocktoken:x>' $U/a/keep.txt)" 204 "UNLOCK"
    expect "$(code -X LOCK --data '<D:lockinfo xmlns:D="DAV:"><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockinfo>' $U/a/new.txt)" 201 "LOCK on a new name"
    expect "$(code -X PROPFIND -H 'Depth: 0' $U/a/new.txt)" 207 "LOCK created an empty file"

    expect "$(code -X DELETE $U/f)" 204 "DELETE folder (recursive)"
    expect "$(code -X PROPFIND -H 'Depth: 0' $U/f)" 404 "deleted folder gone"
    expect "$(code -X DELETE $U/e)" 204 "DELETE second folder"
    expect "$(code -X DELETE $U/nothing)" 404 "DELETE missing"

    # The CLI gets the disk between requests, and the server sees its changes at once.
    timeout 60 $CLI ls usb:/a >/dev/null; check $? "CLI ls while the server runs $fs"
    $CLI mkdir usb:/fromcli >/dev/null
    code -X PROPFIND -H 'Depth: 1' $U/ >/dev/null; grep -q "<D:href>/fromcli/</D:href>" /tmp/body
    check $? "server sees a CLI change at once $fs"
    kill -TERM $DAVPID; wait $DAVPID; check $? "server exits on SIGTERM $fs"
    [ ! -e $STATE ]; check $? "state file removed $fs"
    fsck_part $fs "after WebDAV"
done

echo "=== --secret and the end of the server when the bridge goes away"
$DAV $DEV --secret > /tmp/dav.out 2> /tmp/dav.log &
DAVPID=$!
for i in $(seq 1 100); do [ -s $STATE ] && break; sleep 0.1; done
URL=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["http"])' $STATE)
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' $STATE)
expect "$(code -X PROPFIND -H 'Depth: 1' http://127.0.0.1:$PORT/)" 404 "--secret: / not served"
expect "$(code -X PROPFIND -H 'Depth: 1' $URL)" 207 "--secret: the secret path is"
grep -q "<D:href>${URL#http://127.0.0.1:$PORT}a/</D:href>" /tmp/body; check $? "--secret: hrefs below the secret"
$DAV $DEV --stop; ! kill -0 $DAVPID 2>/dev/null; check $? "--stop"
$DAV $DEV > /tmp/dav.out 2> /tmp/dav.log &
DAVPID=$!
for i in $(seq 1 100); do [ -s $STATE ] && break; sleep 0.1; done
kill $BRIDGE; wait $BRIDGE 2>/dev/null
for i in $(seq 1 30); do kill -0 $DAVPID 2>/dev/null || break; sleep 1; done
! kill -0 $DAVPID 2>/dev/null; check $? "server ends by itself when the device is gone"

if [ -n "${GVFS:-}" ]; then
    echo "=== gvfs (gio) as the client"
    python3 /t/fake_bridge.py /tmp/disk.img ${BS:-512} > /tmp/bridge.log 2>&1 &
    sleep 1
    command -v gio >/dev/null && [ -x /usr/libexec/gvfsd-dav ] || apt-get install -y -qq gvfs gvfs-backends libglib2.0-bin dbus >/dev/null 2>&1
    $CLI format ext4 --yes >/dev/null
    $DAV $DEV --port 8765 > /tmp/dav.out 2> /tmp/dav.log &
    for i in $(seq 1 100); do [ -s $STATE ] && break; sleep 0.1; done
    dbus-run-session -- bash /t/dav_gio.sh
    check $? "gio session"
    fsck_part ext4 "after gio"
fi

[ $fail = 0 ] && echo DAV_ALL_OK || echo DAV_SOME_FAILED
