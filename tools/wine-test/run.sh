#!/bin/bash
# Runs inside debian:trixie (x86_64 is fine): droiddesk-wine and wine-install.sh with wine,
# wineserver, apt-get and dpkg-query stubbed. Checks syntax (sh -n, bash -n, shellcheck),
# argument parsing, path refusal, first-run steps, HODLL selection (global / per app / flag),
# COM links, Start Menu sync, uninstall, and a full install/reinstall from file:// artifacts.
#
#   docker run --rm -v "$PWD/tools/wine-test:/t:ro" -v "$PWD/app/android/app/src/main:/src:ro" \
#       debian:trixie bash /t/run.sh          # ends with WINE_ALL_OK
set -u
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
if ! command -v shellcheck >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq shellcheck procps curl >/dev/null 2>&1 ||
        echo "note: shellcheck not installed (offline?)"
fi
A=/src/assets/droiddesk
W="dash $A/droiddesk-wine"
fail=0
check() { if [ "$1" -ne 0 ]; then echo "FAIL: $2"; fail=1; else echo "ok: $2"; fi; }
has() { grep -qF -- "$2" "$1"; }

# --- syntax ---------------------------------------------------------------------------------
sh -n "$A/droiddesk-wine"; check $? "sh -n droiddesk-wine"
dash -n "$A/droiddesk-wine"; check $? "dash -n droiddesk-wine"
bash -n "$A/wine-install.sh"; check $? "bash -n wine-install.sh"
for f in "$A/droiddesk-wine" "$A/wine-install.sh"; do
    ! grep -q $'\r' "$f"; check $? "LF line endings: $(basename "$f")"
done
if command -v shellcheck >/dev/null; then
    shellcheck -s sh -S warning "$A/droiddesk-wine"; check $? "shellcheck droiddesk-wine"
    shellcheck -s bash -S warning "$A/wine-install.sh"; check $? "shellcheck wine-install.sh"
fi

# --- stubs ----------------------------------------------------------------------------------
S=/tmp/stub
rm -rf "$S" /tmp/root /root/.wine-fex* /root/.config/droiddesk-wine /tmp/ttyUSB*
mkdir -p "$S" /tmp/root/etc/droiddesk /tmp/root/usr/local/sbin
LOG=/tmp/wine.log
export WINELOG=$LOG
cat > "$S/wine" <<'EOF'
#!/bin/sh
# Fake wine: one log line per call with the environment the wrapper chose.
printf '%s\n' "ARGS=$*|HODLL=${HODLL-}|HODLL64=${HODLL64-}|LANG=${LANG-}|LC_ALL=${LC_ALL-}|WINEDEBUG=${WINEDEBUG-unset}|OVR=${WINEDLLOVERRIDES-}|PFX=${WINEPREFIX-}|PWD=$PWD|GPU=${GALLIUM_DRIVER-}" >> "$WINELOG"
case "$1" in
    --version) echo wine-11.16 ;;
    wineboot) mkdir -p "$WINEPREFIX/dosdevices" "$WINEPREFIX/drive_c"; echo stub > "$WINEPREFIX/system.reg" ;;
esac
exit 0
EOF
printf '#!/bin/sh\necho "WINESERVER $*" >> "$WINELOG"\n' > "$S/wineserver"
printf '#!/bin/sh\necho "NOTIFY $*" >> "$WINELOG"\n' > "$S/notify-send"
cat > /tmp/root/usr/local/sbin/droiddesk-menu-sync <<'EOF'
#!/bin/sh
OUT="/tmp/termux-home/.local/share/applications"
echo "MENUSYNC" >> "$WINELOG"
EOF
chmod 755 "$S"/* /tmp/root/usr/local/sbin/droiddesk-menu-sync
export PATH="$S:$PATH" DROIDDESK_WINE_ROOT=/tmp/root
unset DISPLAY
last() { tail -n 1 "$LOG"; }
lastwine() { grep '^ARGS=' "$LOG" | grep -v 'ARGS=reg ' | tail -n 1; }
: > "$LOG"

# --- argument parsing -----------------------------------------------------------------------
$W --help >/dev/null 2>&1; check $? "--help exits 0"
$W --backend box64 doctor >/dev/null 2>/tmp/err; [ $? -eq 2 ] && has /tmp/err "není v této verzi"; check $? "backend box64 refused in v1"
$W --x86 qemu foo.exe >/dev/null 2>/tmp/err; [ $? -eq 2 ]; check $? "--x86 qemu refused"
$W --bogus >/dev/null 2>&1; [ $? -eq 2 ]; check $? "unknown option exit 2"

# --- path handling --------------------------------------------------------------------------
$W /data/data/com.orailnoor.droiddesk/files/home/Downloads/app.exe 2>/tmp/err; rc=$?
[ $rc -eq 2 ] && has /tmp/err "Termux" && has /tmp/err "/storage/emulated/0/Download"; check $? "Termux-side path refused with hint"
[ ! -s "$LOG" ]; check $? "refused path never starts wine"
$W /storage/emulated/0/nothere.exe 2>/tmp/err; [ $? -eq 2 ] && has /tmp/err "neexistuje"; check $? "missing file exit 2"
mkdir -p /storage/emulated/0/Download /root/apps
touch /storage/emulated/0/Download/PuTTY.exe /root/apps/tool.exe /root/apps/setup.msi /root/apps/x.lnk

# --- first run ------------------------------------------------------------------------------
mkdir -p /tmp/root/opt/droiddesk-wine/mono
touch /tmp/root/opt/droiddesk-wine/mono/wine-mono-11.3.0-arm64.msi
$W /sdcard/Download/PuTTY.exe -serial COM3 2>/tmp/err; check $? "/sdcard path runs"
grep -q 'ARGS=wineboot -i|.*OVR=winemenubuilder.exe=d;mscoree=d;mshtml=d' "$LOG"; check $? "first run: wineboot -i with mscoree/mshtml off"
grep -q 'NOTIFY .*první spuštění' "$LOG"; check $? "first run: notify-send note"
grep -qF 'ARGS=msiexec /i Z:\tmp\root\opt\droiddesk-wine\mono\wine-mono-11.3.0-arm64.msi /qn' "$LOG"; check $? "wine-mono MSI installed"
[ "$(cat /root/.wine-fex/.droiddesk-mono)" = wine-mono-11.3.0-arm64.msi ]; check $? "mono stamp"
[ "$(readlink /root/.wine-fex/dosdevices/com3)" = /tmp/ttyUSB0 ] && [ "$(readlink /root/.wine-fex/dosdevices/com4)" = /tmp/ttyUSB1 ]; check $? "default COM3/COM4 links"
[ "$(readlink /root/.wine-fex/dosdevices/d:)" = /storage/emulated/0 ]; check $? "D: -> /storage/emulated/0"
l=$(lastwine)
case "$l" in "ARGS=/storage/emulated/0/Download/PuTTY.exe -serial COM3|"*) true ;; *) false ;; esac; check $? "app started with its args ($l)"
echo "$l" | grep -q '|HODLL=libwow64fex.dll|HODLL64=libarm64ecfex.dll|'; check $? "default x86 = FEX"
echo "$l" | grep -q '|LANG=en_US.UTF-8|LC_ALL=en_US.UTF-8|WINEDEBUG=-all|OVR=winemenubuilder.exe=d|PFX=/root/.wine-fex|PWD=/storage/emulated/0/Download|'; check $? "env: LANG, WINEDEBUG, overrides, prefix, cwd"
grep -q 'NOTIFY .*Sériový port není otevřený (COM3 -> /tmp/ttyUSB0, COM4 -> /tmp/ttyUSB1)' "$LOG"; check $? "missing serial port warned"
grep -q '^WINESERVER -w' "$LOG" && grep -q '^MENUSYNC' "$LOG"; check $? "after run: wineserver -w + menu sync"

: > "$LOG"
touch /tmp/ttyUSB0 /tmp/ttyUSB1
$W run /root/apps/tool.exe 2>/dev/null; check $? "second run"
! grep -q 'wineboot\|msiexec' "$LOG"; check $? "second run: no wineboot, no mono"
! grep -q 'Sériový' "$LOG"; check $? "no port warning when ports exist"

# --- x86 emulator choice --------------------------------------------------------------------
$W x86 box64 >/dev/null; check $? "x86 box64 (global)"
grep -qx 'X86_EMU=box64' /tmp/root/etc/droiddesk/wine.conf; check $? "wine.conf X86_EMU=box64"
$W /root/apps/tool.exe 2>/dev/null; lastwine | grep -q '|HODLL=wowbox64.dll|HODLL64=libarm64ecfex.dll|'; check $? "global box64 -> wowbox64.dll"
$W x86 fex 'C:\Program Files\Tool\TOOL.EXE' >/dev/null; check $? "x86 fex per app"
grep -qx 'tool.exe=fex' /root/.config/droiddesk-wine/apps; check $? "apps file entry"
$W /root/apps/tool.exe 2>/dev/null; lastwine | grep -q '|HODLL=libwow64fex.dll|'; check $? "per-app fex beats global box64"
$W --x86 box64 /root/apps/tool.exe 2>/dev/null; lastwine | grep -q '|HODLL=wowbox64.dll|'; check $? "--x86 beats per-app"
DROIDDESK_WINE_X86=box64 $W /root/apps/tool.exe 2>/dev/null; lastwine | grep -q '|HODLL=wowbox64.dll|'; check $? "env beats per-app"
$W x86 | grep -q 'tool.exe=fex'; check $? "x86 lists overrides"
$W x86 default tool.exe >/dev/null; ! grep -q tool.exe /root/.config/droiddesk-wine/apps; check $? "x86 default removes override"
$W x86 qemu 2>/dev/null; [ $? -eq 2 ]; check $? "x86 qemu refused"
$W x86 fex >/dev/null; grep -qx 'X86_EMU=fex' /tmp/root/etc/droiddesk/wine.conf && [ "$(grep -c '^X86_EMU=' /tmp/root/etc/droiddesk/wine.conf)" = 1 ]; check $? "x86 fex replaces the line"

# --- locale, debug, gpu ---------------------------------------------------------------------
echo 'LOCALE=cs_CZ' >> /tmp/root/etc/droiddesk/wine.conf
printf 'export GALLIUM_DRIVER=zinktest\n' > /tmp/root/etc/droiddesk/gpu-env.sh
DROIDDESK_WINE_DEBUG=+seh $W /root/apps/tool.exe 2>/dev/null
lastwine | grep -q '|LANG=cs_CZ.UTF-8|LC_ALL=cs_CZ.UTF-8|WINEDEBUG=+seh|.*|GPU=zinktest$'; check $? "Czech locale, WINEDEBUG passthrough, gpu-env sourced"
DROIDDESK_WINE_DEBUG=1 $W /root/apps/tool.exe 2>/dev/null; lastwine | grep -q '|WINEDEBUG=unset|'; check $? "DROIDDESK_WINE_DEBUG=1 unsets WINEDEBUG"
touch /tmp/root/etc/droiddesk/gpu-off
$W /root/apps/tool.exe 2>/dev/null; lastwine | grep -q '|GPU=$'; check $? "gpu-off: gpu-env not sourced"
rm -f /tmp/root/etc/droiddesk/gpu-off

# --- file types -----------------------------------------------------------------------------
$W /root/apps/setup.msi 2>/dev/null; lastwine | grep -qF 'ARGS=msiexec /i Z:\root\apps\setup.msi|'; check $? ".msi -> msiexec /i Z:\\..."
$W /root/apps/x.lnk 2>/dev/null; lastwine | grep -qF 'ARGS=start /wait /unix /root/apps/x.lnk|'; check $? ".lnk -> start /wait /unix"
$W cfg 2>/dev/null; lastwine | grep -q '^ARGS=winecfg|'; check $? "cfg -> winecfg"
$W 2>/dev/null </dev/null >/tmp/out; lastwine | grep -q '^ARGS=explorer|'; check $? "no args without a terminal -> explorer"
$W kill; last | grep -q '^WINESERVER -k'; check $? "kill -> wineserver -k"

# --- COM ------------------------------------------------------------------------------------
: > "$LOG"
$W com 5 /tmp/ttyUSB2 2>/tmp/err; check $? "com 5 /tmp/ttyUSB2"
[ "$(readlink /root/.wine-fex/dosdevices/com5)" = /tmp/ttyUSB2 ]; check $? "com5 link"
grep -qF 'ARGS=reg add HKLM\Software\Wine\Ports /v COM5 /t REG_SZ /d /tmp/ttyUSB2 /f' "$LOG"; check $? "Ports registry value"
has /tmp/err "zatím neexistuje"; check $? "missing target reported"
$W com 1 2>/dev/null; [ "$(readlink /root/.wine-fex/dosdevices/com1)" = /tmp/ttyUSB0 ]; check $? "com 1 -> /tmp/ttyUSB0"
$W com 6 /root/x 2>/dev/null; [ $? -eq 2 ] && [ ! -e /root/.wine-fex/dosdevices/com6 ]; check $? "com target outside /tmp,/dev refused"
$W com abc 2>/dev/null; [ $? -eq 2 ]; check $? "com abc refused"
$W com 300 2>/dev/null; [ $? -eq 2 ]; check $? "com 300 refused"

# --- Start Menu sync ------------------------------------------------------------------------
SM="/root/.wine-fex/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs"
UM="/root/.wine-fex/drive_c/users/root/AppData/Roaming/Microsoft/Windows/Start Menu/Programs"
mkdir -p "$SM/PuTTY" "$UM/Nástroje"
touch "$SM/PuTTY/PuTTY.lnk" "$SM/PuTTY/Uninstall PuTTY.lnk" "$UM/Nástroje/Modbus 100%.lnk" "$UM/Nástroje/bad\$name.lnk"
$W menu-sync >/tmp/out; check $? "menu-sync"
cat /tmp/out
n=$(ls /tmp/root/usr/local/share/applications/wine-fex-*.desktop | wc -l); [ "$n" = 2 ]; check $? "2 entries (uninstall and unsafe name skipped), got $n"
f=$(grep -l '^Name=PuTTY$' /tmp/root/usr/local/share/applications/wine-fex-*.desktop)
grep -qxF "Exec=droiddesk-wine --backend fex \"$SM/PuTTY/PuTTY.lnk\"" "$f"; check $? "Exec quotes the .lnk path"
grep -qxF "Exec=droiddesk-wine --backend fex \"$UM/Nástroje/Modbus 100%%.lnk\"" /tmp/root/usr/local/share/applications/wine-fex-Modbus*.desktop; check $? "% escaped as %%"
rm "$SM/PuTTY/PuTTY.lnk"
$W menu-sync >/dev/null; [ "$(ls /tmp/root/usr/local/share/applications/wine-fex-*.desktop | wc -l)" = 1 ]; check $? "stale entry removed"

# --- doctor ---------------------------------------------------------------------------------
printf '#!/bin/sh\nexit 1\n' > "$S/dpkg-query"; chmod 755 "$S/dpkg-query"
$W doctor >/tmp/out 2>&1; check $? "doctor runs"
grep -q 'Velikost stránky: 4096' /tmp/out && grep -q 'Emulátor x86: fex' /tmp/out && grep -q 'com5 -> /tmp/ttyUSB2' /tmp/out; check $? "doctor content"

# --- uninstall ------------------------------------------------------------------------------
cat > "$S/dpkg-query" <<'EOF'
#!/bin/sh
# Stub: hangover packages "installed" when /tmp/stub/installed exists.
for a; do case "$a" in hangover-*) [ -f /tmp/stub/installed ] && printf 'ii '; exit 0 ;; esac; done
exit 1
EOF
printf '#!/bin/sh\necho "APT $*" >> "$WINELOG"\ncase "$1" in install) touch /tmp/stub/installed ;; purge) rm -f /tmp/stub/installed ;; esac\nexit 0\n' > "$S/apt-get"
chmod 755 "$S/dpkg-query" "$S/apt-get"
touch /tmp/stub/installed
mkdir -p /tmp/root/usr/local/bin && cp "$A/droiddesk-wine" /tmp/root/usr/local/bin/droiddesk-wine
printf '[Default Applications]\ntext/plain=mousepad.desktop;\napplication/x-msi=droiddesk-wine.desktop;\n' > /tmp/mime.list
echo 'MIMEAPPS_TERMUX=/tmp/mime.list' >> /tmp/root/etc/droiddesk/wine.conf
: > "$LOG"
$W uninstall --purge-prefix 2>/tmp/err; check $? "uninstall --purge-prefix"
grep -q '^APT purge -y hangover-wine hangover-libarm64ecfex hangover-libwow64fex hangover-wowbox64' "$LOG"; check $? "purges the 4 packages"
[ ! -e /root/.wine-fex ] && ls -d /root/.wine-fex.removed-* >/dev/null 2>&1; check $? "prefix archived, not deleted"
[ ! -e /tmp/root/opt/droiddesk-wine ] && [ ! -e /tmp/root/etc/droiddesk/wine.conf ] && [ ! -e /tmp/root/usr/local/bin/droiddesk-wine ]; check $? "opt, conf, wrapper removed"
grep -q mousepad /tmp/mime.list && ! grep -q droiddesk-wine /tmp/mime.list; check $? "only our MIME defaults removed"
! ls /tmp/root/usr/local/share/applications/wine-fex-* >/dev/null 2>&1; check $? "menu entries removed"

# --- wine-install.sh ------------------------------------------------------------------------
I="bash $A/wine-install.sh"
export DROIDDESK_WINE_SKIP_ARCH=1 TMPDIR=/tmp/insttmp
mkdir -p /tmp/insttmp
rm -rf /tmp/root /tmp/art; mkdir -p /tmp/root/usr/local/sbin /tmp/art/t
printf '#!/bin/sh\nOUT="/tmp/termux-home/.local/share/applications"\necho MENUSYNC >> "$WINELOG"\n' > /tmp/root/usr/local/sbin/droiddesk-menu-sync
chmod 755 /tmp/root/usr/local/sbin/droiddesk-menu-sync
mkdir -p /tmp/termux-home/.config
for p in hangover-wine_11.16~trixie hangover-libwow64fex_11.16 hangover-libarm64ecfex_11.16 hangover-wowbox64_11.16; do
    echo fake > "/tmp/art/t/${p}_arm64.deb"
done
echo fake > /tmp/art/t/dxvk-v2.7.1.tar.gz
tar -C /tmp/art/t -cf /tmp/art/hangover_11.16_debian13_trixie_arm64.tar .
echo mono > /tmp/art/wine-mono-11.3.0-arm64.msi
HS=$(sha256sum /tmp/art/hangover_11.16_debian13_trixie_arm64.tar | cut -d' ' -f1)
MS=$(sha256sum /tmp/art/wine-mono-11.3.0-arm64.msi | cut -d' ' -f1)
ARGS=(--hangover file:///tmp/art/hangover_11.16_debian13_trixie_arm64.tar "$HS" --mono file:///tmp/art/wine-mono-11.3.0-arm64.msi "$MS")
rm -f /tmp/stub/installed

$I --hangover x 123 --mono y 456 >/dev/null 2>&1; [ $? -ne 0 ]; check $? "install: bad sha256 refused"
cat > "$S/dpkg-query" <<'EOF'
#!/bin/sh
case "$*" in *"wine*"*) printf 'wine 10.0~repack-6 ii \nlibwine 10.0~repack-6 ii \nwine64-tools 10.0 un \n'; exit 0 ;; esac
exit 1
EOF
: > "$LOG"
$I "${ARGS[@]}" >/tmp/out 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q '  wine 10.0~repack-6' /tmp/out && grep -q '  libwine' /tmp/out && ! grep -q 'wine64-tools' /tmp/out; check $? "install: Debian wine packages listed and refused"
! grep -q '^APT' "$LOG"; check $? "install: nothing installed after refusal"
cat > "$S/dpkg-query" <<'EOF'
#!/bin/sh
case "$*" in *"wine*"*) exit 1 ;; esac
for a; do case "$a" in hangover-*) [ -f /tmp/stub/installed ] && printf 'ii '; exit 0 ;; esac; done
exit 1
EOF
: > "$LOG"
$I "${ARGS[@]}" --locale cs_CZ --slim >/tmp/out 2>&1; rc=$?
tail -n 3 /tmp/out
[ $rc -eq 0 ]; check $? "install from file:// artifacts"
grep -q '^APT update' "$LOG" && grep -q '^APT -y -o Dpkg::Options::=--force-confold upgrade' "$LOG"; check $? "apt-get update + upgrade before install"
grep -q '^APT install -y --allow-downgrades /tmp/root/opt/droiddesk-wine/hangover.new/hangover-wine_11.16~trixie_arm64.deb .*hangover-wowbox64_11.16_arm64.deb fonts-liberation2 fonts-dejavu-core' "$LOG"; check $? "apt-get install of the 4 debs"
! grep -q 'APT install.*dxvk' "$LOG"; check $? "dxvk not installed"
O=/tmp/root/opt/droiddesk-wine
[ -f $O/dxvk/dxvk-v2.7.1.tar.gz ] && [ -f $O/mono/wine-mono-11.3.0-arm64.msi ] && ! ls $O/hangover/*.deb >/dev/null 2>&1 && [ ! -e $O/hangover.new ] && [ ! -e $O/hangover.old ]; check $? "/opt layout (dxvk kept, debs dropped, no .new/.old)"
grep -q "^hangover_11.16_debian13_trixie_arm64 $HS\$" $O/hangover/.droiddesk-version; check $? "hangover .droiddesk-version"
grep -qx 'path-exclude=/usr/include/wine/\*' /tmp/root/etc/dpkg/dpkg.cfg.d/droiddesk-wine-slim; check $? "--slim dpkg path-exclude"
C=/tmp/root/etc/droiddesk/wine.conf
grep -qx 'X86_EMU=fex' $C && grep -qx 'LOCALE=cs_CZ' $C && grep -qx 'MIMEAPPS_TERMUX=/tmp/termux-home/.config/mimeapps.list' $C; check $? "wine.conf"
[ -x /tmp/root/usr/local/bin/droiddesk-wine ] && cmp -s /tmp/root/usr/local/bin/droiddesk-wine "$A/droiddesk-wine"; check $? "wrapper installed"
D=/tmp/root/usr/local/share/applications
grep -qx 'Name=Spustit ve Windows (Wine)' $D/droiddesk-wine.desktop && grep -qx 'Name=Nastavení Windows (Wine)' $D/droiddesk-winecfg.desktop && grep -qx 'Name=Soubory Windows' $D/droiddesk-wine-explorer.desktop; check $? "desktop files (Czech names)"
grep -qx 'application/x-ms-dos-executable=debian-droiddesk-wine.desktop;' /tmp/termux-home/.config/mimeapps.list && grep -qx 'application/x-msi=droiddesk-wine.desktop;' /tmp/root/etc/xdg/mimeapps.list; check $? "MIME defaults (Termux + Debian)"
grep -q '^MENUSYNC' "$LOG"; check $? "droiddesk-menu-sync called"
[ -s /tmp/root/var/log/droiddesk-wine-install.log ]; check $? "install log written"

# Re-run: user choice kept, packages not reinstalled, MIME entries not duplicated.
DROIDDESK_WINE_ROOT=/tmp/root dash /tmp/root/usr/local/bin/droiddesk-wine x86 box64 >/dev/null
printf '[Default Applications]\ntext/plain=mousepad.desktop;\n[Added Associations]\nfoo/bar=x.desktop;\n' > /tmp/termux-home/.config/mimeapps.list
: > "$LOG"
$I "${ARGS[@]}" >/tmp/out 2>&1; check $? "re-run"
! grep -q '^APT' "$LOG"; check $? "re-run: no apt when up to date"
grep -qx 'X86_EMU=box64' $C && grep -qx 'LOCALE=cs_CZ' $C; check $? "re-run keeps X86_EMU and LOCALE"
[ "$(grep -c 'application/x-msi=' /tmp/termux-home/.config/mimeapps.list)" = 1 ] && grep -q '^foo/bar=x.desktop;' /tmp/termux-home/.config/mimeapps.list && grep -q mousepad /tmp/termux-home/.config/mimeapps.list; check $? "MIME merge keeps other entries"
awk '/^\[Added/{a=1} a && /x-msi/{bad=1} END{exit bad}' /tmp/termux-home/.config/mimeapps.list; check $? "MIME entries land in [Default Applications]"
: > "$LOG"
$I "${ARGS[@]}" --force >/tmp/out 2>&1; check $? "--force"
grep -q '^APT install -y --allow-downgrades --reinstall' "$LOG"; check $? "--force reinstalls"
[ ! -e /tmp/root/etc/dpkg/dpkg.cfg.d/droiddesk-wine-slim ]; check $? "without --slim the path-exclude file is removed"

echo
if [ $fail -eq 0 ]; then echo WINE_ALL_OK; else echo WINE_FAILED; exit 1; fi
