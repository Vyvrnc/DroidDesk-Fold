#!/bin/bash
# shellcheck disable=SC1111  # Czech quotes („…“) in messages are intended
# craft-install.sh: Craft desktop apps (storytold, Rust/wgpu) in DroidDesk's Debian (trixie arm64).
#
#   craft-install.sh install ID     latest release from GitHub (= update when installed)
#   craft-install.sh uninstall ID
#   craft-install.sh status ID      prints the installed tag, or "not-installed"
#   craft-install.sh latest ID      prints TAG DEB SHA256 of the latest release, installs nothing
#   craft-install.sh list           ID OWNER/REPO per line
#
# install: GitHub API releases/latest -> ID-<ver>-linux-aarch64.deb + SHA256SUMS.txt of the same
# release -> sha256 check (missing or different = stop) -> apt-get install ./file.deb (resolves
# libvulkan1, libasound2t64 and the rest) -> /etc/droiddesk/craft/ID.version. Versions are not
# pinned: storytold releases almost daily, the checksum file of the release is the anchor.
# The menu entry goes through gpu-run (Vulkan over Turnip even while droiddesk-gpu is off) and
# ALSA's default device becomes PulseAudio (the apps talk ALSA, sound reaches Android via pulse).
# Office apps also get Carlito/Caladea/Inconsolata for Calibri/Cambria/Consolas in documents.
# Lines "craft-install: [n/N] …" are progress for the app; "craft-install: CHYBA: …" is the error.
set -euo pipefail

ROOT=${DROIDDESK_CRAFT_ROOT:-}
STATE=$ROOT/etc/droiddesk/craft
APPDIR=$ROOT/usr/local/share/applications
ASOUND=$ROOT/etc/asound.conf
MENU_SYNC=$ROOT/usr/local/sbin/droiddesk-menu-sync
GPU_RUN=$ROOT/usr/local/bin/gpu-run
API=${DROIDDESK_CRAFT_API:-https://api.github.com}
WEB=${DROIDDESK_CRAFT_WEB:-https://github.com}
WORK=${TMPDIR:-/tmp}/droiddesk-craft-install
ASOUND_MARK='# droiddesk-craft: ALSA -> PulseAudio'
STEPS=5

say() { printf 'craft-install: %s\n' "$*"; }
step() { printf 'craft-install: [%s/%s] %s\n' "$1" "$STEPS" "$2"; }
die() { printf 'craft-install: CHYBA: %s\n' "$*" >&2; exit 1; }
usage() { sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

repo_of() {
    case "$1" in
        photocraft|lightcraft|filmcraft|wordcraft|gridcraft|deckcraft|pdfcraft|vectorcraft|\
        designcraft|effectcraft|soundcraft|figcraft|cadcraft) echo "storytold/$1" ;;
        solidcraft) echo "tangency-labs/solidcraft" ;;
        *) return 1 ;;
    esac
}
ALL_IDS="photocraft lightcraft filmcraft wordcraft gridcraft deckcraft pdfcraft vectorcraft designcraft effectcraft soundcraft figcraft cadcraft solidcraft"

[ $# -ge 1 ] || usage
ACTION=$1
if [ "$ACTION" = list ]; then
    for id in $ALL_IDS; do echo "$id $(repo_of "$id")"; done
    exit 0
fi
[ $# -eq 2 ] || usage
ID=$2
REPO=$(repo_of "$ID") || die "neznámá aplikace „${ID}“ (seznam: craft-install.sh list)"

installed_tag() {
    if dpkg-query -W -f='${Status}' "$ID" 2>/dev/null | grep -q 'install ok installed'; then
        cat "$STATE/$ID.version" 2>/dev/null || dpkg-query -W -f='v${Version}' "$ID"
    fi
}

menu_sync() {
    if [ -x "$MENU_SYNC" ]; then "$MENU_SYNC" || say "synchronizace menu selhala (aplikace je nainstalovaná)"; fi
}

# Debian's .desktop with Exec through gpu-run, in /usr/local/share/applications under the same
# name: droiddesk-menu-sync reads that directory after /usr/share/applications, so this copy
# wins in the XFCE menu.
write_menu_override() {
    local src out
    src=$(dpkg-query -L "$ID" 2>/dev/null | grep -E '^/usr/share/applications/[^/]+\.desktop$' | head -1 || true)
    [ -n "$src" ] && [ -f "$ROOT$src" ] || { say "balík nemá .desktop soubor, položka menu chybí"; return 0; }
    out=$APPDIR/$(basename "$src")
    if [ ! -x "$GPU_RUN" ]; then
        rm -f "$out"
        say "gpu-run chybí (starší debian-setup), aplikace poběží s výchozím prostředím"
        return 0
    fi
    mkdir -p "$APPDIR"
    sed -E 's|^Exec=|Exec=/usr/local/bin/gpu-run |; /^TryExec=/d' "$ROOT$src" > "$out.tmp"
    printf 'X-DroidDesk-Craft=%s\n' "$ID" >> "$out.tmp"
    mv "$out.tmp" "$out"
}

remove_menu_override() {
    local f
    for f in "$APPDIR"/*.desktop; do
        [ -f "$f" ] || continue
        grep -qx "X-DroidDesk-Craft=$ID" "$f" && rm -f "$f"
    done
    return 0
}

# ALSA's default device -> PulseAudio (PULSE_SERVER is set in Debian). Added once with a marker;
# a default the user defined is left alone.
ensure_alsa_pulse() {
    if ! dpkg-query -W -f='${Status}' libasound2-plugins 2>/dev/null | grep -q 'install ok installed'; then
        apt-get install -y -o DPkg::Lock::Timeout=300 libasound2-plugins ||
            say "libasound2-plugins se nepodařilo nainstalovat, zvuk nemusí fungovat"
    fi
    if [ -f "$ASOUND" ] && grep -qF "$ASOUND_MARK" "$ASOUND"; then return 0; fi
    if [ -f "$ASOUND" ] && grep -qE '^[[:space:]]*(pcm|ctl)\.!?default' "$ASOUND"; then
        say "$ASOUND už má vlastní výchozí zařízení, nechávám ho být"
        return 0
    fi
    mkdir -p "$(dirname "$ASOUND")"
    {
        [ -s "$ASOUND" ] && echo
        echo "$ASOUND_MARK (craft-install.sh; delete the block to undo)"
        echo 'pcm.!default { type pulse }'
        echo 'ctl.!default { type pulse }'
    } >> "$ASOUND"
}

# Craft apps draw through wgpu (Vulkan). Debian's lavapipe is the CPU fallback gpu-run lists after
# Turnip, so the apps still start where Turnip does not come up (ScreenMate One, Adreno 610).
ensure_vulkan_fallback() {
    dpkg-query -W -f='${Status}' mesa-vulkan-drivers 2>/dev/null | grep -q 'install ok installed' && return 0
    apt-get install -y -o DPkg::Lock::Timeout=300 mesa-vulkan-drivers ||
        say "mesa-vulkan-drivers se nepodařilo nainstalovat, bez GPU aplikace nemusí naběhnout"
}

# Office documents name Calibri, Cambria and Consolas: Carlito and Caladea are metric-compatible
# (Debian's fontconfig aliases them), Inconsolata stands in for Consolas through our alias.
# Only missing ones are installed; uninstall leaves them (other apps share them).
OFFICE_FONTS="fonts-crosextra-carlito fonts-crosextra-caladea fonts-inconsolata"
FONT_ALIAS=$ROOT/etc/fonts/conf.d/60-droiddesk-consolas.conf
ensure_office_fonts() {
    case "$ID" in wordcraft|gridcraft|deckcraft|pdfcraft) ;; *) return 0 ;; esac
    local p missing=()
    for p in $OFFICE_FONTS; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' || missing+=("$p")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        apt-get install -y -o DPkg::Lock::Timeout=300 "${missing[@]}" ||
            say "písma ${missing[*]} se nepodařilo nainstalovat, dokumenty se zobrazí náhradním písmem"
    fi
    if [ ! -f "$FONT_ALIAS" ]; then
        mkdir -p "$(dirname "$FONT_ALIAS")"
        printf '%s\n' '<?xml version="1.0"?>' '<!DOCTYPE fontconfig SYSTEM "fonts.dtd">' \
            '<!-- craft-install.sh: Consolas in documents -> Inconsolata -->' '<fontconfig>' \
            '  <alias binding="same"><family>Consolas</family><accept><family>Inconsolata</family></accept></alias>' \
            '</fontconfig>' > "$FONT_ALIAS"
        if command -v fc-cache >/dev/null; then fc-cache -f >/dev/null 2>&1 || true; fi
    fi
    return 0
}

# GET URL -> stdout; HTTP code in $HTTP_CODE. Network errors die, HTTP errors are the caller's.
HTTP_CODE=
fetch() {
    local out=$1 url=$2 code
    code=$(curl -sSL --connect-timeout 20 --retry 2 -o "$out" -w '%{http_code}' \
        -H 'Accept: application/vnd.github+json' -H 'User-Agent: DroidDesk-craft-install' "$url") ||
        die "nelze se spojit s GitHubem (${url%%/repos/*}). Zkontrolujte připojení k internetu."
    HTTP_CODE=$code
}

# Fills TAG, DEB_NAME, DEB_URL, SUMS_URL.
TAG='' DEB_NAME='' DEB_URL='' SUMS_URL=''
resolve_latest() {
    local json=$WORK/release.json names
    fetch "$json" "$API/repos/$REPO/releases/latest"
    case "$HTTP_CODE" in
        200) ;;
        403|429)
            if grep -qi 'rate limit' "$json"; then
                say "limit GitHub API vyčerpán, zkouším přesměrování releases/latest"
                resolve_latest_web
                return
            fi
            die "GitHub odmítl dotaz na $REPO (HTTP $HTTP_CODE)." ;;
        404) die "$REPO nemá žádné vydání (HTTP 404)." ;;
        *) die "GitHub API vrátilo HTTP $HTTP_CODE pro $REPO." ;;
    esac
    TAG=$(grep -m1 -oE '"tag_name"[[:space:]]*:[[:space:]]*"[^"]+"' "$json" | sed -E 's/.*"([^"]+)"$/\1/' || true)
    [ -n "$TAG" ] || die "odpověď GitHub API pro $REPO neobsahuje tag_name."
    names=$(grep -oE '"browser_download_url"[[:space:]]*:[[:space:]]*"[^"]+"' "$json" | sed -E 's/.*"([^"]+)"$/\1/' || true)
    # The versioned name first (solidcraft also ships an unversioned copy of the same file).
    DEB_URL=$(printf '%s\n' "$names" | grep -E "/${ID}-[0-9][^/]*-linux-aarch64\.deb$" | head -1 || true)
    [ -n "$DEB_URL" ] || DEB_URL=$(printf '%s\n' "$names" | grep -E '/[^/]*-linux-aarch64\.deb$' | head -1 || true)
    [ -n "$DEB_URL" ] || die "vydání $TAG ($REPO) nemá balík .deb pro linux-aarch64."
    DEB_NAME=${DEB_URL##*/}
    SUMS_URL=$(printf '%s\n' "$names" | grep -E '/SHA256SUMS\.txt$' | head -1 || true)
    [ -n "$SUMS_URL" ] || die "vydání $TAG ($REPO) nemá SHA256SUMS.txt, bez kontrolního součtu neinstaluji."
}

# Without the API (rate limit): github.com/OWNER/REPO/releases/latest redirects to .../tag/TAG,
# the .deb name comes from SHA256SUMS.txt of that tag.
resolve_latest_web() {
    local loc sums=$WORK/SHA256SUMS.txt
    loc=$(curl -sS --connect-timeout 20 -o /dev/null -w '%{redirect_url}' "$WEB/$REPO/releases/latest") ||
        die "nelze se spojit s GitHubem. Zkontrolujte připojení k internetu."
    TAG=${loc##*/tag/}
    [ -n "$loc" ] && [ "$TAG" != "$loc" ] ||
        die "limit GitHub API vyčerpán a poslední vydání $REPO nelze zjistit. Zkuste to za hodinu."
    SUMS_URL=$WEB/$REPO/releases/download/$TAG/SHA256SUMS.txt
    fetch "$sums" "$SUMS_URL"
    [ "$HTTP_CODE" = 200 ] || die "vydání $TAG ($REPO) nemá SHA256SUMS.txt (HTTP $HTTP_CODE), bez kontrolního součtu neinstaluji."
    DEB_NAME=$(awk '{n=$2; sub(/^\*/, "", n); print n}' "$sums" | grep -E "^${ID}-[0-9].*-linux-aarch64\.deb$" | head -1 || true)
    [ -n "$DEB_NAME" ] || DEB_NAME=$(awk '{n=$2; sub(/^\*/, "", n); print n}' "$sums" | grep -E -- '-linux-aarch64\.deb$' | head -1 || true)
    [ -n "$DEB_NAME" ] || die "vydání $TAG ($REPO) nemá balík .deb pro linux-aarch64."
    DEB_URL=$WEB/$REPO/releases/download/$TAG/$DEB_NAME
}

# sha256 of DEB_NAME from SHA256SUMS.txt of the resolved release ("HASH  NAME" or "HASH *NAME").
expected_sha() {
    local sums=$WORK/SHA256SUMS.txt want
    if [ ! -s "$sums" ]; then
        fetch "$sums" "$SUMS_URL"
        [ "$HTTP_CODE" = 200 ] || die "SHA256SUMS.txt vydání $TAG nelze stáhnout (HTTP $HTTP_CODE)."
    fi
    want=$(awk -v n="$DEB_NAME" '{f=$2; sub(/^\*/, "", f); if (f == n) {print tolower($1); exit}}' "$sums")
    [[ $want =~ ^[0-9a-f]{64}$ ]] || die "SHA256SUMS.txt vydání $TAG neobsahuje $DEB_NAME, bez kontrolního součtu neinstaluji."
    echo "$want"
}

ensure_curl() {
    if ! command -v curl >/dev/null || [ ! -f /etc/ssl/certs/ca-certificates.crt ]; then
        [ "$(id -u)" = 0 ] || die "chybí curl a bez roota ho nelze doinstalovat."
        apt-get update -qq || true
        apt-get install -y -qq curl ca-certificates || die "nelze nainstalovat curl. Zkontrolujte připojení k internetu."
    fi
    rm -rf "$WORK"; mkdir -p "$WORK"
    trap 'rm -rf "$WORK"' EXIT
}

do_latest() {
    ensure_curl
    resolve_latest
    local want
    want=$(expected_sha)
    echo "$TAG $DEB_NAME $want"
}

do_install() {
    [ "$(id -u)" = 0 ] || die "spusťte jako root (debian-run v DroidDesku je root)"
    if [ -z "${DROIDDESK_CRAFT_SKIP_ARCH:-}" ]; then
        local arch
        arch=$(dpkg --print-architecture)
        [ "$arch" = arm64 ] || die "Craft aplikace jsou tu jen pro arm64 (tady $arch)."
    fi
    export DEBIAN_FRONTEND=noninteractive
    ensure_curl

    step 1 "Zjišťuji poslední vydání $REPO..."
    resolve_latest
    local current
    current=$(installed_tag)
    if [ -n "$current" ]; then say "nainstalováno $current, poslední vydání $TAG"; else say "poslední vydání $TAG"; fi

    step 2 "Stahuji $DEB_NAME..."
    local deb=$WORK/$DEB_NAME want got
    want=$(expected_sha)
    fetch "$deb" "$DEB_URL"
    [ "$HTTP_CODE" = 200 ] || die "stažení $DEB_NAME selhalo (HTTP $HTTP_CODE)."

    step 3 "Ověřuji kontrolní součet..."
    got=$(sha256sum "$deb" | cut -d' ' -f1)
    [ "$got" = "$want" ] || die "kontrolní součet $DEB_NAME nesouhlasí (očekáváno $want, staženo $got). Soubor zahozen."
    say "sha256 v pořádku ($got)"

    step 4 "Instaluji $DEB_NAME a závislosti..."
    if ! apt-get install -y -o DPkg::Lock::Timeout=300 "$deb"; then
        say "instalace selhala, obnovuji seznam balíků a zkouším znovu"
        apt-get update || die "seznam balíků Debianu nelze obnovit. Zkontrolujte připojení k internetu."
        apt-get install -y -o DPkg::Lock::Timeout=300 "$deb" || die "apt-get nedokázal nainstalovat $DEB_NAME (viz výpis výše)."
    fi
    dpkg-query -W -f='${Status}' "$ID" 2>/dev/null | grep -q 'install ok installed' ||
        die "balík $ID po instalaci chybí (název balíku v .deb je jiný než $ID?)."
    mkdir -p "$STATE"
    echo "$TAG" > "$STATE/$ID.version"

    step 5 "Zvuk, Vulkan, písma a položka v menu..."
    ensure_alsa_pulse
    ensure_vulkan_fallback
    ensure_office_fonts
    write_menu_override
    menu_sync
    say "hotovo: $ID $TAG"
}

do_uninstall() {
    [ "$(id -u)" = 0 ] || die "spusťte jako root (debian-run v DroidDesku je root)"
    export DEBIAN_FRONTEND=noninteractive
    remove_menu_override
    if dpkg-query -W -f='${Status}' "$ID" 2>/dev/null | grep -q 'install ok installed'; then
        apt-get remove -y -o DPkg::Lock::Timeout=300 "$ID" || die "apt-get nedokázal odebrat $ID (viz výpis výše)."
    else
        say "$ID není nainstalovaný"
    fi
    rm -f "$STATE/$ID.version"
    menu_sync
    say "odinstalováno: $ID"
}

case "$ACTION" in
    install) do_install ;;
    uninstall) do_uninstall ;;
    status) tag=$(installed_tag); echo "${tag:-not-installed}" ;;
    latest) do_latest ;;
    *) usage ;;
esac
