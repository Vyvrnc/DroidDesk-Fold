#!/bin/bash
# Inside dbus-run-session (from dav.sh with GVFS=1): the gvfs dav backend against
# droiddesk-usb-dav on port 8765, the way Thunar uses it.
set -u
fail=0
check() { if [ "$1" -ne 0 ]; then echo "FAIL: gio $2"; fail=1; else echo "ok: gio $2"; fi; }
CZ="Příliš žluťoučký kůň.txt"
/usr/libexec/gvfsd >/tmp/gvfsd.log 2>&1 &
sleep 2
R=dav://localhost:8765
timeout 60 gio mount $R/; check $? "mount"
gio mount -l | grep -i dav
timeout 60 gio mkdir $R/docs; check $? "mkdir"
timeout 120 gio copy /tmp/up/big.bin $R/docs/big.bin
check $? "copy 30 MB up"
timeout 60 gio copy "/tmp/up/$CZ" "$R/docs/"; check $? "copy a Czech name up"
timeout 60 gio list -l $R/docs/; check $? "list"
timeout 60 gio list $R/docs/ | grep -qx "$CZ"; check $? "Czech name listed"
rm -f /tmp/gio.back; timeout 120 gio copy $R/docs/big.bin /tmp/gio.back; check $? "copy 30 MB down"
cmp /tmp/up/big.bin /tmp/gio.back; check $? "content identical"
timeout 60 gio rename "$R/docs/$CZ" "Nový název.txt"; check $? "rename"
timeout 60 gio move $R/docs/big.bin $R/big-moved.bin; check $? "move"
# An editor's save: g_file_replace (temporary file + rename, or a direct PUT).
printf 'upraveno\n' | timeout 60 gio save "$R/docs/Nový název.txt"; check $? "save (replace)"
[ "$(timeout 60 gio cat "$R/docs/Nový název.txt")" = upraveno ]; check $? "cat after save"
timeout 60 gio info $R/big-moved.bin | grep -E "standard::size|display-name"
timeout 60 gio remove -f $R/big-moved.bin; check $? "remove file"
timeout 60 gio trash $R/docs 2>/dev/null; timeout 60 gio remove -f "$R/docs/Nový název.txt"; timeout 60 gio remove $R/docs
check $? "remove folder"
timeout 60 gio list $R/; check $? "list root"
gio mount -u $R/
exit $fail
