#!/bin/bash
# shellcheck disable=SC1111  # Czech quotes („…“) in messages are intended
# wine-install.sh: installs or repairs "Windows aplikace" in DroidDesk's Debian (trixie arm64).
# v1 = backend "fex": Hangover (aarch64 Wine with ARM64EC, FEX + Box64 as PE DLLs) and
# wine-mono for .NET apps. See docs/windows-apps-plan.md.
#
#   wine-install.sh --hangover URL SHA256 --mono URL SHA256 [--version LABEL]
#                   [--locale ll_CC] [--termux-home DIR] [--wrapper FILE] [--slim] [--force]
#
# URLs are passed in (upstream or our mirror is decided by the caller); http(s):// and
# file:// work. Idempotent: same artifacts already installed = only the wrapper, config,
# menu and MIME files are rewritten; --force reinstalls the packages (repair).
# Log: /var/log/droiddesk-wine-install.log.
set -euo pipefail

ROOT=${DROIDDESK_WINE_ROOT:-}
OPT=$ROOT/opt/droiddesk-wine
CONF=$ROOT/etc/droiddesk/wine.conf
APPDIR=$ROOT/usr/local/share/applications
BIN=$ROOT/usr/local/bin/droiddesk-wine
DPKG_SLIM=$ROOT/etc/dpkg/dpkg.cfg.d/droiddesk-wine-slim
MENU_SYNC=$ROOT/usr/local/sbin/droiddesk-menu-sync
LOG=${DROIDDESK_WINE_INSTALL_LOG:-$ROOT/var/log/droiddesk-wine-install.log}
WORK=${TMPDIR:-/tmp}/droiddesk-wine-install
DEBS=(hangover-wine hangover-libarm64ecfex hangover-libwow64fex hangover-wowbox64)
MIME_TYPES="application/x-ms-dos-executable;application/x-msdownload;application/vnd.microsoft.portable-executable;application/x-msi;application/x-ms-shortcut;application/x-bat;"
# Disk estimate (MB, Phase 0/1): hangover-wine 2430 installed + the tar 280 + its unpacked
# debs 290 while installing + prefix 2600 + wine-mono 90 (+ ~300 in the prefix).
NEED_FRESH_MB=6000
NEED_UPGRADE_MB=1200

say() { printf '%s\n' "$*"; }
step() { printf '\n=== %s ===\n' "$*"; }
die() { printf 'wine-install: CHYBA: %s\n' "$*" >&2; exit 1; }
usage() { sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

HANGOVER_URL='' HANGOVER_SHA='' MONO_URL='' MONO_SHA='' VERSION='' LOCALE='' TERMUX_HOME='' WRAPPER='' SLIM='' FORCE=''
while [ $# -gt 0 ]; do
    case "$1" in
        --hangover) [ $# -ge 3 ] || usage; HANGOVER_URL=$2; HANGOVER_SHA=$3; shift 3 ;;
        --mono) [ $# -ge 3 ] || usage; MONO_URL=$2; MONO_SHA=$3; shift 3 ;;
        --version) [ $# -ge 2 ] || usage; VERSION=$2; shift 2 ;;
        --locale) [ $# -ge 2 ] || usage; LOCALE=$2; shift 2 ;;
        --termux-home) [ $# -ge 2 ] || usage; TERMUX_HOME=$2; shift 2 ;;
        --wrapper) [ $# -ge 2 ] || usage; WRAPPER=$2; shift 2 ;;
        --slim) SLIM=1; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage ;;
        *) printf 'wine-install: neznámý argument „%s“\n' "$1" >&2; usage ;;
    esac
done
[ -n "$HANGOVER_URL" ] && [ -n "$MONO_URL" ] || usage
for s in "$HANGOVER_SHA" "$MONO_SHA"; do
    [[ $s =~ ^[0-9a-f]{64}$ ]] || die "neplatný sha256 „${s}“"
done
[ -n "$VERSION" ] || VERSION=$(basename "$HANGOVER_URL" .tar)
WRAPPER=${WRAPPER:-$(dirname "$(readlink -f "$0")")/droiddesk-wine}
[ -f "$WRAPPER" ] || die "chybí wrapper $WRAPPER"

mkdir -p "$(dirname "$LOG")"
exec > >(tee -a "$LOG") 2>&1
say "--- wine-install $(date '+%Y-%m-%d %H:%M:%S') verze $VERSION ${SLIM:+--slim }${FORCE:+--force}"

[ "$(id -u)" = 0 ] || die "spusť jako root (debian-run v DroidDesku je root)"
if [ -z "${DROIDDESK_WINE_SKIP_ARCH:-}" ]; then
    arch=$(dpkg --print-architecture)
    [ "$arch" = arm64 ] || die "Hangover je jen pro arm64 (tady $arch)"
fi
ps=$(getconf PAGESIZE 2>/dev/null || echo 4096)
[ "$ps" = 4096 ] || die "jádro má stránky $ps B; Wine potřebuje 4096 B (16KB jádro nepodporujeme)"

step "1/9 Kontrola: Wine z Debianu nesmí být nainstalované"
# Hangover's hangover-wine conflicts with Debian's wine; libwine/wine64-tools overlap
# /usr/include/wine and winegcc. Never remove the user's packages ourselves.
conflicts=$(dpkg-query -W -f '${Package} ${Version} ${db:Status-Abbrev}\n' 'wine*' 'libwine*' fex-emu-wine 2>/dev/null |
    awk '$3 ~ /^[ih]i/ {print "  " $1 " " $2}' || true)
if [ -n "$conflicts" ]; then
    say "Nainstalované balíčky Wine z Debianu:"
    say "$conflicts"
    die "odeber je (apt-get purge …) a spusť instalaci znovu; jinak by se s Hangoverem praly o /usr/bin/wine"
fi
say "ok"

installed_sha=$(awk '{print $2}' "$OPT/hangover/.droiddesk-version" 2>/dev/null || true)
pkgs_ok=1
for p in "${DEBS[@]}"; do
    dpkg-query -W -f '${db:Status-Abbrev}' "$p" 2>/dev/null | grep -q '^ii' || pkgs_ok=
done
need_hangover=1
if [ -z "$FORCE" ] && [ -n "$pkgs_ok" ] && [ "$installed_sha" = "$HANGOVER_SHA" ]; then
    need_hangover=
fi
mono_name=$(basename "$MONO_URL")
need_mono=1
if [ -z "$FORCE" ] && [ "$(cat "$OPT/mono/.droiddesk-version" 2>/dev/null || true)" = "$mono_name $MONO_SHA" ] &&
    [ -f "$OPT/mono/$mono_name" ]; then
    need_mono=
fi

step "2/9 Volné místo"
if [ -n "$pkgs_ok" ]; then need=$NEED_UPGRADE_MB; else need=$NEED_FRESH_MB; fi
[ -n "$need_hangover" ] || need=200
for dir in "$ROOT/" "${TMPDIR:-/tmp}"; do
    free=$(df -Pm "$dir" | awk 'NR==2 {print $4}')
    say "$dir: volných $free MB, potřeba ~$need MB"
    [ "$free" -ge "$need" ] || die "málo místa v $dir: $free MB volných, potřeba ~$need MB (Wine ~2,4 GB + prefix ~2,6 GB)"
done

mkdir -p "$WORK" "$OPT"
fetch() {
    # fetch URL SHA256 FILE: download and verify; a verified file from an interrupted run is reused.
    if [ -f "$3" ] && echo "$2  $3" | sha256sum -c - >/dev/null 2>&1; then
        say "$(basename "$3"): už stažené, kontrolní součet sedí"
        return 0
    fi
    rm -f "$3"
    say "Stahuji $1"
    curl -fL --retry 3 --retry-delay 5 -o "$3.part" "$1" || die "stažení selhalo: $1"
    echo "$2  $3.part" | sha256sum -c - || { rm -f "$3.part"; die "kontrolní součet nesedí: $1"; }
    mv -f "$3.part" "$3"
}

if [ -n "$need_hangover" ]; then
    step "3/9 apt-get update && apt-get -y upgrade"
    # hangover-wine needs libavcodec61/libavformat61/libavutil59 >= 7:7.1.5 (Debian 13.x point
    # release); an older rootfs must be brought up to date first.
    export DEBIAN_FRONTEND=noninteractive
    apt-get update || die "apt-get update selhal (internet?)"
    apt-get -y -o Dpkg::Options::=--force-confold upgrade || die "apt-get upgrade selhal"

    step "4/9 Stažení Hangover ($VERSION)"
    fetch "$HANGOVER_URL" "$HANGOVER_SHA" "$WORK/hangover.tar"

    step "5/9 Rozbalení do $OPT/hangover.new"
    rm -rf "$OPT/hangover.new"
    mkdir -p "$OPT/hangover.new"
    tar -xf "$WORK/hangover.tar" -C "$OPT/hangover.new" || { rm -rf "$OPT/hangover.new"; die "rozbalení selhalo"; }
    files=()
    for p in "${DEBS[@]}"; do
        f=$(ls "$OPT/hangover.new/${p}"_*.deb 2>/dev/null | head -n 1 || true)
        [ -n "$f" ] || { rm -rf "$OPT/hangover.new"; die "v archivu chybí ${p}_*.deb"; }
        files+=("$f")
    done

    step "6/9 Instalace balíčků (${DEBS[*]})"
    mkdir -p "$(dirname "$DPKG_SLIM")"
    if [ -n "$SLIM" ]; then
        # Development parts only (headers, import libraries, winegcc/widl/wrc…), checked against
        # the hangover-wine 11.16 file list: ~128 MB of 2.4 GB. dpkg skips them at unpack time.
        cat > "$DPKG_SLIM" <<'EOF'
# droiddesk-wine --slim: Wine's development files are not installed
path-exclude=/usr/include/wine/*
path-exclude=/usr/lib/wine/aarch64-windows/*.a
path-exclude=/usr/lib/wine/i386-windows/*.a
path-exclude=/usr/bin/widl
path-exclude=/usr/bin/winebuild
path-exclude=/usr/bin/winedump
path-exclude=/usr/bin/winegcc
path-exclude=/usr/bin/wineg++
path-exclude=/usr/bin/winecpp
path-exclude=/usr/bin/winemaker
path-exclude=/usr/bin/wmc
path-exclude=/usr/bin/wrc
path-exclude=/usr/bin/function_grep.pl
EOF
        say "--slim: vývojové soubory Wine se neinstalují"
    else
        rm -f "$DPKG_SLIM"
    fi
    reinstall=()
    [ -n "$FORCE" ] && reinstall=(--reinstall)
    if ! apt-get install -y --allow-downgrades "${reinstall[@]}" "${files[@]}" fonts-liberation2 fonts-dejavu-core; then
        rm -rf "$OPT/hangover.new"
        die "instalace balíčků selhala (předchozí verze zůstává, pokud byla)"
    fi

    step "7/9 Přepnutí $OPT/hangover"
    # Debs are not needed after installation (repair = download again); DXVK is kept for a
    # later "droiddesk-wine dxvk on" but not enabled.
    rm -rf "$OPT/dxvk.new"
    if ls "$OPT"/hangover.new/dxvk-*.tar.gz >/dev/null 2>&1; then
        mkdir -p "$OPT/dxvk.new"
        mv "$OPT"/hangover.new/dxvk-*.tar.gz "$OPT/dxvk.new/"
        basename "$(ls "$OPT"/dxvk.new/dxvk-*.tar.gz)" .tar.gz > "$OPT/dxvk.new/.droiddesk-version"
        rm -rf "$OPT/dxvk.old"
        [ -d "$OPT/dxvk" ] && mv "$OPT/dxvk" "$OPT/dxvk.old"
        mv "$OPT/dxvk.new" "$OPT/dxvk"
        rm -rf "$OPT/dxvk.old"
    fi
    rm -f "$OPT"/hangover.new/*.deb
    echo "$VERSION $HANGOVER_SHA" > "$OPT/hangover.new/.droiddesk-version"
    rm -rf "$OPT/hangover.old"
    [ -d "$OPT/hangover" ] && mv "$OPT/hangover" "$OPT/hangover.old"
    mv "$OPT/hangover.new" "$OPT/hangover"
    rm -rf "$OPT/hangover.old"
    rm -f "$WORK/hangover.tar"
else
    say "Hangover $VERSION už je nainstalovaný (kroky 3–7 přeskočeny; oprava: --force)"
fi

step "8/9 Wine Mono ($mono_name)"
if [ -n "$need_mono" ]; then
    fetch "$MONO_URL" "$MONO_SHA" "$WORK/$mono_name"
    rm -rf "$OPT/mono.new"
    mkdir -p "$OPT/mono.new"
    mv "$WORK/$mono_name" "$OPT/mono.new/"
    echo "$mono_name $MONO_SHA" > "$OPT/mono.new/.droiddesk-version"
    rm -rf "$OPT/mono.old"
    [ -d "$OPT/mono" ] && mv "$OPT/mono" "$OPT/mono.old"
    mv "$OPT/mono.new" "$OPT/mono"
    rm -rf "$OPT/mono.old"
    say "připraveno; do prefixu se nainstaluje při dalším spuštění aplikace"
else
    say "už je připravené"
fi
rmdir "$WORK" 2>/dev/null || true

step "9/9 Wrapper, konfigurace, menu"
mkdir -p "$(dirname "$BIN")"
# CR stripped in case the asset ever passes through a Windows checkout.
tr -d '\r' < "$WRAPPER" > "$BIN.new"
chmod 755 "$BIN.new"
mv -f "$BIN.new" "$BIN"
say "$BIN"

# Termux home (for Thunar's mimeapps.list): argument, else from droiddesk-menu-sync's OUT=.
if [ -z "$TERMUX_HOME" ] && [ -f "$MENU_SYNC" ]; then
    TERMUX_HOME=$(sed -n 's|^OUT="\(.*\)/\.local/share/applications"$|\1|p' "$MENU_SYNC" | head -n 1)
fi
TERMUX_MIMEAPPS=
[ -n "$TERMUX_HOME" ] && [ -d "$TERMUX_HOME" ] && TERMUX_MIMEAPPS=$TERMUX_HOME/.config/mimeapps.list

# The user's x86 emulator choice survives a reinstall.
x86=$(sed -n 's/^X86_EMU=//p' "$CONF" 2>/dev/null | tail -n 1 || true)
case "$x86" in fex|box64) ;; *) x86=fex ;; esac
[ -n "$LOCALE" ] || LOCALE=$(sed -n 's/^LOCALE=//p' "$CONF" 2>/dev/null | tail -n 1 || true)
mkdir -p "$(dirname "$CONF")"
cat > "$CONF.new" <<EOF
# droiddesk-wine; written by wine-install.sh ($(date +%Y-%m-%d)). X86_EMU is kept on reinstall,
# change it with: droiddesk-wine x86 fex|box64
BACKEND=fex
VERSION=$VERSION
X86_EMU=$x86
LOCALE=$LOCALE
MIMEAPPS_TERMUX=$TERMUX_MIMEAPPS
EOF
mv -f "$CONF.new" "$CONF"
say "$CONF (X86_EMU=$x86${LOCALE:+, LOCALE=$LOCALE})"

mkdir -p "$APPDIR"
write_desktop() {
    # write_desktop FILE NAME COMMENT EXEC ICON [MimeType]
    {
        echo "[Desktop Entry]"
        echo "Type=Application"
        echo "Name=$2"
        echo "Comment=$3"
        echo "Exec=$4"
        echo "Icon=$5"
        echo "Categories=System;"
        echo "Terminal=false"
        if [ -n "${6:-}" ]; then echo "MimeType=$6"; fi
    } > "$APPDIR/$1"
}
write_desktop droiddesk-wine.desktop "Spustit ve Windows (Wine)" \
    "Spustí program pro Windows (.exe, .msi, .lnk)" "droiddesk-wine %f" \
    application-x-ms-dos-executable "$MIME_TYPES"
write_desktop droiddesk-winecfg.desktop "Nastavení Windows (Wine)" \
    "Nastavení Wine: verze Windows, jednotky, zvuk, grafika" "droiddesk-wine cfg" preferences-system
write_desktop droiddesk-wine-explorer.desktop "Soubory Windows" \
    "Průzkumník souborů prostředí Windows (Wine)" "droiddesk-wine explorer" system-file-manager

mime_set() {
    # mime_set FILE DESKTOP: our MIME types -> DESKTOP in [Default Applications]; other
    # entries and sections stay.
    local f=$1 d=$2
    mkdir -p "$(dirname "$f")"
    [ -f "$f" ] || : > "$f"
    awk -v d="$d" -v types="$MIME_TYPES" '
        BEGIN { n = split(types, t, ";"); for (i = 1; i <= n; i++) if (t[i] != "") want[t[i]] = 1 }
        function emit() { for (i = 1; i <= n; i++) if (t[i] != "") print t[i] "=" d ";"; done = 1 }
        /^\[/ { if (insec && !done) emit(); insec = ($0 == "[Default Applications]"); print; next }
        insec { split($0, kv, "="); if (kv[1] in want) next }
        { print }
        END { if (!done) { if (!insec) print "[Default Applications]"; emit() } }
    ' "$f" > "$f.new" && mv -f "$f.new" "$f"
    say "MIME výchozí aplikace: $f"
}
# Debian-side apps; Thunar (Termux side) opens the mirrored debian-droiddesk-wine.desktop.
mime_set "$ROOT/etc/xdg/mimeapps.list" droiddesk-wine.desktop
if [ -n "$TERMUX_MIMEAPPS" ]; then
    mime_set "$TERMUX_MIMEAPPS" debian-droiddesk-wine.desktop
else
    say "Domov Termuxu neznámý (--termux-home), Thunar zatím .exe neotevře dvojklikem"
fi

if [ -x "$MENU_SYNC" ]; then
    "$MENU_SYNC" || say "droiddesk-menu-sync selhal (menu se obnoví při dalším apt)"
fi

say ""
say "Hotovo: Windows aplikace ($VERSION). První spuštění programu připraví prostředí (~1–2 min)."
say "Diagnostika: droiddesk-wine doctor"
