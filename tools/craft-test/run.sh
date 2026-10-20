#!/bin/bash
# Runs inside debian:trixie (x86_64 is fine): craft-install.sh against the real GitHub API with
# apt-get and dpkg-query stubbed (the stub unpacks the .deb with dpkg-deb into a fake root, so the
# menu override is built from the real .desktop file). Checks syntax (bash -n, shellcheck), latest
# release resolution for every app, a real install/update/uninstall of gridcraft (~19 MB),
# the rate-limit fallback over github.com redirects, and the refusals (checksum mismatch, no
# aarch64 .deb, no SHA256SUMS.txt, HTTP 500, no network, unknown app, user's own ALSA default).
#
#   docker run --rm -v "$PWD/tools/craft-test:/t:ro" -v "$PWD/app/android/app/src/main:/src:ro" \
#       debian:trixie bash /t/run.sh          # ends with CRAFT_ALL_OK
# Unauthenticated GitHub API: ~20 requests of the 60 per hour.
set -u
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
apt-get update -qq >/dev/null && apt-get install -y -qq shellcheck curl ca-certificates python3 >/dev/null 2>&1 ||
    echo "note: test tools not installed (offline?)"
A=/src/assets/droiddesk
C="bash $A/craft-install.sh"
fail=0
check() { if [ "$1" -ne 0 ]; then echo "FAIL: $2"; fail=1; else echo "ok: $2"; fi; }
has() { grep -qF -- "$2" "$1"; }

# --- syntax ---------------------------------------------------------------------------------
bash -n "$A/craft-install.sh"; check $? "bash -n craft-install.sh"
! grep -q $'\r' "$A/craft-install.sh"; check $? "LF line endings"
if command -v shellcheck >/dev/null; then
    shellcheck -s bash -S warning "$A/craft-install.sh"; check $? "shellcheck craft-install.sh"
fi

# --- stubs ----------------------------------------------------------------------------------
S=/tmp/stub R=/tmp/root D=/tmp/dpkg-state
rm -rf "$S" "$R" "$D"; mkdir -p "$S" "$R/usr/local/sbin" "$R/usr/local/bin" "$D"
export APTLOG=/tmp/apt.log DPKG_STATE=$D FAKE_ROOT=$R
: > "$APTLOG"
cat > "$S/apt-get" <<'EOF'
#!/bin/bash
# Fake apt-get: logs; "install ./x.deb" unpacks into FAKE_ROOT and records the package.
echo "APT $*" >> "$APTLOG"
action=
for a; do
    case "$a" in
        install|remove|update) action=$a ;;
        *.deb) pkg=$(dpkg-deb -f "$a" Package) || exit 100
            dpkg-deb -x "$a" "$FAKE_ROOT"
            dpkg-deb -c "$a" | awk '{print substr($6, 2)}' | sed 's|/$||' > "$DPKG_STATE/$pkg.list"
            dpkg-deb -f "$a" Version > "$DPKG_STATE/$pkg.installed" ;;
        -*|DPkg::*) ;;
        *) if [ "$action" = install ]; then echo 1 > "$DPKG_STATE/$a.installed"
           elif [ "$action" = remove ]; then rm -f "$DPKG_STATE/$a.installed"; fi ;;
    esac
done
exit 0
EOF
cat > "$S/dpkg-query" <<'EOF'
#!/bin/bash
# Fake dpkg-query: -W -f=FORMAT PKG and -L PKG from DPKG_STATE.
fmt= list=
for a; do
    case "$a" in
        -L) list=1 ;;
        -W) ;;
        -f=*) fmt=${a#-f=} ;;
        *) pkg=$a ;;
    esac
done
[ -f "$DPKG_STATE/$pkg.installed" ] || exit 1
if [ -n "$list" ]; then cat "$DPKG_STATE/$pkg.list"; exit 0; fi
case "$fmt" in
    *Status*) printf 'install ok installed' ;;
    *Version*) printf 'v%s' "$(cat "$DPKG_STATE/$pkg.installed")" ;;
esac
EOF
cat > "$R/usr/local/sbin/droiddesk-menu-sync" <<'EOF'
#!/bin/sh
echo "MENUSYNC" >> "$APTLOG"
EOF
printf '#!/bin/sh\n. /etc/droiddesk/gpu-env.sh\nexec "$@"\n' > "$R/usr/local/bin/gpu-run"
chmod 755 "$S"/* "$R/usr/local/sbin/droiddesk-menu-sync" "$R/usr/local/bin/gpu-run"
export PATH="$S:$PATH" DROIDDESK_CRAFT_ROOT=$R DROIDDESK_CRAFT_SKIP_ARCH=1
echo 1 > "$D/libasound2-plugins.installed"

# --- latest release of every app (real API) --------------------------------------------------
for id in $($C list | cut -d' ' -f1); do
    line=$($C latest "$id" 2>&1)
    read -r tag deb sha <<< "$line"
    [[ $tag =~ ^v[0-9] && $deb =~ ^$id-[0-9][^/]*-linux-aarch64\.deb$ && $sha =~ ^[0-9a-f]{64}$ ]]
    check $? "latest $id: $line"
done
[ "$($C list | wc -l)" = 14 ]; check $? "14 apps in the table"

# --- install / update / uninstall gridcraft (real download) ---------------------------------
read -r gtag gdeb _ <<< "$($C latest gridcraft)"
out=$($C install gridcraft 2>&1); rc=$?
echo "$out" | tail -n 4
check $rc "install gridcraft"
echo "$out" | grep -q '^craft-install: \[5/5\]'; check $? "progress lines [n/5]"
echo "$out" | grep -q 'sha256 v pořádku'; check $? "sha256 verified"
grep -q "^APT install -y -o DPkg::Lock::Timeout=300 /tmp/droiddesk-craft-install/$gdeb$" "$APTLOG"; check $? "apt-get install ./$gdeb"
[ "$(cat "$R/etc/droiddesk/craft/gridcraft.version")" = "$gtag" ]; check $? "version file = $gtag"
[ "$($C status gridcraft)" = "$gtag" ]; check $? "status = $gtag"
O=$R/usr/local/share/applications/ai.storyteller.gridcraft.desktop
has "$O" 'Exec=/usr/local/bin/gpu-run gridcraft %F'; check $? "menu override runs gpu-run"
! grep -q '^TryExec=' "$O"; check $? "override without TryExec"
has "$O" 'X-DroidDesk-Craft=gridcraft'; check $? "override marked"
has "$O" 'Icon=ai.storyteller.gridcraft'; check $? "override keeps the icon"
has "$R/etc/asound.conf" 'pcm.!default { type pulse }'; check $? "asound.conf pcm -> pulse"
has "$R/etc/asound.conf" 'ctl.!default { type pulse }'; check $? "asound.conf ctl -> pulse"
grep -q MENUSYNC "$APTLOG"; check $? "menu sync ran"
grep -q '^APT install -y -o DPkg::Lock::Timeout=300 fonts-crosextra-carlito fonts-crosextra-caladea fonts-inconsolata$' "$APTLOG"
check $? "office fonts installed for gridcraft"
has "$R/etc/fonts/conf.d/60-droiddesk-consolas.conf" '<family>Consolas</family><accept><family>Inconsolata</family>'
check $? "Consolas -> Inconsolata alias"
[ ! -e /tmp/droiddesk-craft-install ]; check $? "download removed"
$C install gridcraft > /dev/null 2>&1; check $? "install again (= update)"
[ "$(grep -c droiddesk-craft "$R/etc/asound.conf")" = 1 ]; check $? "ALSA block added once"
[ "$(grep -c 'fonts-crosextra' "$APTLOG")" = 1 ]; check $? "fonts not reinstalled on update"
: > "$APTLOG"
$C uninstall gridcraft > /dev/null 2>&1; check $? "uninstall gridcraft"
grep -q '^APT remove -y -o DPkg::Lock::Timeout=300 gridcraft$' "$APTLOG"; check $? "apt-get remove gridcraft"
[ ! -e "$O" ]; check $? "override removed"
[ ! -e "$R/etc/droiddesk/craft/gridcraft.version" ]; check $? "version file removed"
[ "$($C status gridcraft)" = not-installed ]; check $? "status = not-installed"
has "$R/etc/asound.conf" 'type pulse'; check $? "asound.conf kept after uninstall"
! grep -q 'remove.*fonts-' "$APTLOG" && [ -f "$D/fonts-inconsolata.installed" ]; check $? "fonts kept after uninstall"

# --- mock API: fallback and refusals -----------------------------------------------------------
python3 -I /t/mock_github.py 8765 & mock=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null http://127.0.0.1:8765/ && break; sleep 0.3; done
M=http://127.0.0.1:8765
line=$(DROIDDESK_CRAFT_API=$M/rl $C latest solidcraft 2>&1)
echo "$line" | grep -q 'limit GitHub API vyčerpán'; check $? "rate limit noticed"
echo "$line" | tail -n 1 | grep -qE '^v[0-9][^ ]* solidcraft-[0-9][^ ]*-linux-aarch64\.deb [0-9a-f]{64}$'
check $? "rate limit -> redirect fallback picks the versioned solidcraft .deb: $(echo "$line" | tail -n 1)"
: > "$APTLOG"
out=$(DROIDDESK_CRAFT_API=$M/bad $C install gridcraft 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q 'CHYBA: kontrolní součet gridcraft-9.9.9-linux-aarch64.deb nesouhlasí'
check $? "checksum mismatch refused"
! grep -q '^APT install' "$APTLOG"; check $? "nothing installed after mismatch"
[ ! -e "$R/etc/droiddesk/craft/gridcraft.version" ]; check $? "no version file after mismatch"
out=$(DROIDDESK_CRAFT_API=$M/nodeb $C install gridcraft 2>&1)
echo "$out" | grep -q 'CHYBA: vydání v9.9.9 (storytold/gridcraft) nemá balík .deb pro linux-aarch64'; check $? "no aarch64 .deb refused"
out=$(DROIDDESK_CRAFT_API=$M/nosums $C install gridcraft 2>&1)
echo "$out" | grep -q 'CHYBA: vydání v9.9.9 (storytold/gridcraft) nemá SHA256SUMS.txt'; check $? "no SHA256SUMS.txt refused"
out=$(DROIDDESK_CRAFT_API=$M/e500 $C install gridcraft 2>&1)
echo "$out" | grep -q 'CHYBA: GitHub API vrátilo HTTP 500'; check $? "HTTP 500 refused"
out=$(DROIDDESK_CRAFT_API=http://127.0.0.1:9 $C install gridcraft 2>&1)
echo "$out" | grep -q 'CHYBA: nelze se spojit s GitHubem'; check $? "no network refused"
out=$($C install photoshop 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q 'CHYBA: neznámá aplikace „photoshop“'; check $? "unknown app refused"
kill "$mock" 2>/dev/null

# --- ALSA: a default the user defined stays -------------------------------------------------
printf 'pcm.!default { type hw card 0 }\n' > "$R/etc/asound.conf"
( set -e; ID=x ASOUND=$R/etc/asound.conf ASOUND_MARK='# droiddesk-craft: ALSA -> PulseAudio'
  eval "$(sed -n '/^say()/p; /^ensure_alsa_pulse()/,/^}/p' "$A/craft-install.sh")"
  ensure_alsa_pulse )
[ "$(cat "$R/etc/asound.conf")" = 'pcm.!default { type hw card 0 }' ]; check $? "user's ALSA default untouched"

[ $fail -eq 0 ] && echo CRAFT_ALL_OK || { echo CRAFT_FAILED; exit 1; }
