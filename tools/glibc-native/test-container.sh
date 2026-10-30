#!/usr/bin/env bash
set -euo pipefail
apt-get update -qq
apt-get install -y -qq --no-install-recommends gcc libc6-dev patchelf python3
work=$(mktemp -d /data-glibc-native.XXXXXX)
trap 'rm -rf "$work"' EXIT
root=$work/rootfs
mkdir -p "$root"/{etc,var,usr/bin,root,home/test,usr/share/X11/xkb,opt/search/probe} "$work"/{tmp,plib} /system/bin
ln -s usr/bin "$root/bin"
printf 'mapped\n' > "$root/etc/native-test"
printf 'root fixture\n' > "$root/root/x"
printf 'home fixture\n' > "$root/home/test/x"
ln -s /missing-xkb-target "$root/usr/share/X11/xkb/broken"
cat > "$root/etc/passwd" <<'PASSWD'
malformed-passwd-entry
dd-alice:x:42420:42420:Alice Example:/home/alice:/bin/bash
dd-bob:x:42421:42421:Bob Example:/home/bob:/bin/sh
PASSWD
cat > "$root/etc/group" <<'GROUP'
malformed-group-entry
dd-primary:x:42420:dd-alice
dd-shared:x:42422:dd-alice,dd-bob
dd-shared-duplicate:x:42422:dd-alice
dd-other:x:42423:dd-bob
GROUP
ln -s /etc/native-test "$root/etc/absolute-file"
ln -s /etc "$root/opt/absolute-dir"
ln -s ../etc "$root/opt/relative-dir"
ln -s /usr/bin/probe "$root/usr/bin/absolute-probe"
ln -s /etc/loop-b "$root/etc/loop-a"
ln -s /etc/loop-a "$root/etc/loop-b"
printf '#!/bin/sh -e\nexec /usr/bin/probe child script\n' > "$root/usr/bin/script"
chmod +x "$root/usr/bin/script"
cp /bin/dash "$root/usr/bin/sh"
gcc -shared -fPIC -O2 -Wall -Wextra -Werror -o "$work/hook.so" /repo/tools/glibc-native/pathhook.c -ldl
gcc -O2 -Wall -Wextra -Werror -o "$root/usr/bin/probe" /repo/tools/glibc-native/test.c -ldl -pthread
mkdir -p "$root/root/apps/test"
cp "$root/usr/bin/probe" "$root/root/apps/test/AppRun"
cp "$root/usr/bin/probe" "$root/home/test/home-probe"
printf 'int native_symbol = 42;\n' | gcc -shared -fPIC -x c - -o "$root/opt/native-test.so"
case $(uname -m) in
  x86_64) loader=/lib64/ld-linux-x86-64.so.2; libs=/lib/x86_64-linux-gnu ;;
  aarch64) loader=/lib/ld-linux-aarch64.so.1; libs=/lib/aarch64-linux-gnu ;;
  *) exit 1 ;;
esac
ln -s "$loader" "$work/plib/ld.so"
ln -s "$loader" /system/bin/linker64
cp "$root/usr/bin/probe" "$root/usr/bin/android-marker"
patchelf --set-interpreter /system/bin/linker64 "$root/usr/bin/android-marker"
cp "$root/usr/bin/probe" "$root/usr/bin/patched"
# Preserve the device patchelf order and hook persistence across a direct exec.
patchelf --force-rpath --set-rpath "$libs" "$root/usr/bin/patched"
patchelf --set-interpreter "$work/plib/ld.so" "$root/usr/bin/patched"
patchelf --add-needed "$work/hook.so" "$root/usr/bin/patched"
export DD_ROOTFS=$root DD_TMP=$work/tmp DD_PLIB=$work/plib DD_LIBPATH=$libs DD_NATIVE_ACTIVE=1
export DD_SAVED_PREFIX=/termux/usr DD_SAVED_TERMUX_VERSION=0.118 DD_SAVED_TERMUX__TEST="value with spaces"
export DD_SAVED_PATH=/termux/bin DD_SAVED_LD_LIBRARY_PATH=$libs DD_SAVED_LD_PRELOAD=$work/empty.so
printf 'int marker;\n' | gcc -shared -fPIC -x c - -o "$work/empty.so"
export PATH=/usr/bin:/bin
exec 63<"$root/etc/native-test"
"$loader" --library-path "$libs" --preload "$work/hook.so" "$root/usr/bin/probe"
"$loader" --library-path "$libs" --preload "$work/hook.so" "$root/usr/bin/probe" unix
"$loader" --library-path "$libs" --preload "$work/hook.so" "$root/usr/bin/probe" nss
# Self re-exec through /proc/self/exe and through the loader (bun's embedded ripgrep).
gcc -O2 -Wall -Wextra -Werror -o "$root/usr/bin/selfexec" /repo/tools/glibc-native/selfexec.c
for how in proc loader; do
    out=$("$work/plib/ld.so" --library-path "$libs" --preload "$work/hook.so" --argv0 selfexec "$root/usr/bin/selfexec" "$how")
    [[ $out == "selfexec child --no-config exe-ok" ]] || { echo "self re-exec ($how): $out" >&2; exit 1; }
done
echo "self re-exec checks OK"
# Process name from --argv0 instead of "ld.so" (pgrep, pkill).
[[ $("$work/plib/ld.so" --library-path "$libs" --preload "$work/hook.so" --argv0 /opt/x/my-probe "$root/usr/bin/probe" comm) == my-probe ]]
echo "process name checks OK"
# accept under a filter that kills the accept syscall (Android): the hook must use accept4.
gcc -O2 -Wall -Wextra -Werror -o "$work/acceptseccomp" /repo/tools/glibc-native/acceptseccomp.c
[[ $(LD_PRELOAD="$work/hook.so" "$work/acceptseccomp") == "accept ok" ]]
if "$work/acceptseccomp" >/dev/null 2>&1; then echo "accept was not blocked without the hook" >&2; exit 1; fi
echo "accept via accept4 under seccomp checks OK"
# syscall(kcmp) killed and hard links refused, as on Android: ENOSYS and a published copy instead.
gcc -O2 -Wall -Wextra -Werror -o "$work/seccomp2" /repo/tools/glibc-native/seccomp2.c
mkdir -p "$work/linktest"
[[ $(LD_PRELOAD="$work/hook.so" "$work/seccomp2" "$work/linktest") == "seccomp2 ok" ]]
if "$work/seccomp2" "$work/linktest" >/dev/null 2>&1; then echo "kcmp was not blocked without the hook" >&2; exit 1; fi
echo "generic syscall() blocklist and hard link fallback checks OK"
# A mirror program started with a cleaned environment restores the runtime from .dd-env, and
# Debian's session bus replaces the Termux one (Electron re-launch, keyring dialog).
mkdir -p "$work/mirror-env"
cp /usr/bin/printenv "$work/mirror-env/printenv"
printf 'DD_ROOTFS=/rootfs-test\nDBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-debian\n' > "$work/mirror-env/.dd-env"
[[ $(env -i LD_PRELOAD="$work/hook.so" "$work/mirror-env/printenv" DD_ROOTFS DBUS_SESSION_BUS_ADDRESS | tr '\n' ' ') == "/rootfs-test unix:path=/tmp/dbus-debian " ]]
[[ $(env -i DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-session LD_PRELOAD="$work/hook.so" "$work/mirror-env/printenv" DBUS_SESSION_BUS_ADDRESS) == "unix:path=/tmp/dbus-debian" ]]
[[ $(env -i DD_ROOTFS=/x DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-session LD_PRELOAD="$work/hook.so" /usr/bin/printenv DBUS_SESSION_BUS_ADDRESS) == "unix:path=/tmp/dbus-debian" ]]
echo "mirror environment restore checks OK"
# The resolver's own probes must not map themselves again: a deep path took 2^depth stats.
deep=/opt/deep$(printf '/d%s' $(seq 1 26))
mkdir -p "$root$deep"
timeout 10 "$loader" --library-path "$libs" --preload "$work/hook.so" /usr/bin/stat -c %F "$deep" | grep -qx directory
echo "deep path resolution checks OK"
# Static programs (Go, musl) bypass the hook: they run under proot with the same rootfs.
mkdir -p /termux/usr/bin /termux/usr/libexec/proot
printf '#!/bin/sh\necho "loader=$PROOT_LOADER"\nfor a in "$@"; do echo "arg=$a"; done\n' > /termux/usr/bin/proot
chmod +x /termux/usr/bin/proot
printf 'int main(void) { return 0; }\n' | gcc -static -x c - -o "$root/usr/bin/static-tool"
out=$("$loader" --library-path "$libs" --preload "$work/hook.so" "$root/usr/bin/sh" -c 'cd /usr && exec /usr/bin/static-tool one "two words"')
grep -qx "loader=/termux/usr/libexec/proot/loader" <<< "$out"
grep -qx "arg=-r" <<< "$out" && grep -qx "arg=$root" <<< "$out"
grep -qx "arg=-w" <<< "$out" && grep -qx "arg=/usr" <<< "$out"
grep -qx "arg=$work/tmp:/tmp" <<< "$out"
grep -qx "arg=/usr/bin/static-tool" <<< "$out" && grep -qx "arg=two words" <<< "$out"
grep -qx "arg=PATH=/usr/bin:/bin" <<< "$out"
! grep -q "^arg=DD_" <<< "$out"
! grep -q "^arg=/bin/bash" <<< "$out"
# Relative program, physical paths in environment values and arguments, custom argv[0].
cp /bin/bash "$root/usr/bin/bash"
out=$(TMPDIR=$work/tmp/x MYPATH="$root/usr/bin:/opt:$work/tmp" "$loader" --library-path "$libs" --preload "$work/hook.so" \
    "$root/usr/bin/bash" -c "cd /usr/bin && exec -a applet ./static-tool $root/etc/native-test")
grep -qx "arg=/usr/bin/static-tool" <<< "$out"
grep -qx "arg=TMPDIR=/tmp/x" <<< "$out"
grep -qx "arg=MYPATH=/usr/bin:/opt:/tmp" <<< "$out"
grep -qx "arg=/etc/native-test" <<< "$out"
grep -qx "arg=/bin/bash" <<< "$out" && grep -qx "arg=applet" <<< "$out"
echo "static program via proot checks OK"
# Source only launcher functions; exercise the same code as the shipped commands.
(
    export PREFIX=$work/termux
    source <(sed '/^command=/,$d' /repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native)
    R=$root N=$PREFIX/opt/glibc-native T=$work/tmp
    PLIB=$N/plib
    LIBPATH=$PLIB:$N/lib:$libs
    mkdir -p "$N"/{lib,bin,locale,plib} "$PREFIX/bin" "$root/usr/lib/aarch64-linux-gnu"
    ln -s "$loader" "$PLIB/ld.so"
    ln -s "$libs/libc.so.6" "$PLIB/libc.so.6"
    ln -s "$work/hook.so" "$N/lib/pathhook.so"
    ln -s /usr/bin/env "$PREFIX/bin/env"
    cp /usr/bin/env "$root/usr/bin/env"
    cp /bin/bash "$root/usr/bin/bash"
    cp "$root/etc/native-test" "$root/etc/resolv.conf"
    # Bun closes fd 63 when it spawns; the hook reopens resolv.conf in the child.
    [[ $("$loader" --library-path "$libs" --preload "$work/hook.so" /bin/cat /proc/self/fd/63 63<&-) == mapped ]]
    echo "resolv.conf fd restore checks OK"
    # Links for Bun's PATH lookup: relative and alternatives links resolve inside the rootfs.
    mkdir -p "$root/usr/share/npm" "$root/etc/alternatives"
    printf '#!/bin/sh\necho npx-ok "$@"\n' > "$root/usr/share/npm/npx-cli.js"
    chmod +x "$root/usr/share/npm/npx-cli.js"
    ln -s ../share/npm/npx-cli.js "$root/usr/bin/npx"
    ln -s /usr/bin/probe "$root/etc/alternatives/editor"
    ln -s /etc/alternatives/editor "$root/usr/bin/editor"
    printf 'data\n' > "$root/usr/bin/not-executable"
    update_path_links
    [[ $(readlink "$N/path-links/npx") == "$root/usr/share/npm/npx-cli.js" ]]
    [[ $(readlink "$N/path-links/editor") == "$root/usr/bin/probe" ]]
    [[ ! -e $N/path-links/not-executable && -L $N/path-links/sh ]]
    links_inode=$(stat -c %i "$N/path-links")
    update_path_links
    [[ $(stat -c %i "$N/path-links") == "$links_inode" ]]
    [[ $("$loader" --library-path "$libs" --preload "$work/hook.so" "$root/usr/bin/sh" -c "$N/path-links/npx arg") == "npx-ok arg" ]]
    rm "$root/usr/bin/npx"
    update_path_links
    [[ ! -e $N/path-links/npx && -L $N/path-links/editor ]]
    # Changes outside PATH directories and links written with the physical rootfs path.
    mkdir -p "$root/opt/tool/v1" "$root/opt/tool/v2"
    printf '#!/bin/sh\n' | tee "$root/opt/tool/v1/tool" > "$root/opt/tool/v2/tool"
    chmod +x "$root/opt/tool/v1/tool" "$root/opt/tool/v2/tool"
    ln -s /opt/tool/v1 "$root/opt/tool/current"
    ln -s /opt/tool/current/tool "$root/usr/bin/tool"
    ln -s "$root/opt/tool/v1/tool" "$root/usr/bin/physical-tool"
    update_path_links
    [[ $(readlink "$N/path-links/tool") == "$root/opt/tool/v1/tool" ]]
    [[ $(readlink "$N/path-links/physical-tool") == "$root/opt/tool/v1/tool" ]]
    ln -sfn /opt/tool/v2 "$root/opt/tool/current"
    chmod -x "$root/opt/tool/v1/tool"
    update_path_links
    [[ $(readlink "$N/path-links/tool") == "$root/opt/tool/v2/tool" && ! -e $N/path-links/physical-tool ]]
    [[ $(find "$N" -maxdepth 1 -name 'path-links.*' -type d | wc -l) == 1 ]]
    # Parallel launches never publish a partial directory.
    for i in 1 2 3 4; do update_path_links & done; wait
    [[ -L $N/path-links/sh && -L $N/path-links/tool && $(find "$N" -maxdepth 1 -name 'path-links.*' -type d | wc -l) == 1 ]]
    echo "PATH link checks OK"
    touch "$N/stamp"
    mkdir -p "$root/usr/lib/locale/C.utf8"
    ln -s /opt/native-test.so "$root/usr/lib/aarch64-linux-gnu/libnative-alias.so"
    setup_data
    [[ -f $N/libpaths && ! -s $N/libpaths && $LIBPATH != *: ]]
    [[ $(readlink "$N/lib/libnative-alias.so") == "$root/opt/native-test.so" ]]
    DD_LIBPATH=$LIBPATH "$loader" --library-path "$LIBPATH" --preload "$work/hook.so" "$root/usr/bin/probe" library
    [[ $(LANG=xx_YY.UTF-8 native_locale) == C.UTF-8 ]]
    mkdir -p "$N/locale/en_US.UTF-8" "$N/locale/cs_CZ.utf8"
    [[ $(LANG=en_US.UTF-8 native_locale) == en_US.UTF-8 ]]
    [[ $(LANG=cs_CZ.UTF-8 native_locale) == cs_CZ.UTF-8 ]]
    [[ $(LANG=C native_locale) == C ]]
    [[ $(source_path /root) == "$root/root" ]]
    mkdir -p "$root/home/test"
    [[ $(source_path /home/test) == "$root/home/test" ]]
    # Resolve each component before processing its following dot-dot component.
    mkdir -p "$root/opt/resolve-test/target/deep" "$root/opt/resolve-test/space target"
    printf 'resolved\n' > "$root/opt/resolve-test/target/marker"
    printf 'spaces\n' > "$root/opt/resolve-test/space target/file with spaces"
    ln -s /opt/resolve-test/target "$root/opt/resolve-test/absolute"
    ln -s target "$root/opt/resolve-test/relative"
    ln -s target/deep "$root/opt/resolve-test/deep-link"
    ln -s 'space target' "$root/opt/resolve-test/space link"
    resolve_into "$root/opt/resolve-test/absolute/marker"
    [[ $REPLY == "$root/opt/resolve-test/target/marker" ]]
    # A physical link from outside Debian must adopt its root before nested links.
    ln -s "$root/opt/resolve-test/absolute/marker" "$N/external-resolve-link"
    resolve_into "$N/external-resolve-link"
    [[ $REPLY == "$root/opt/resolve-test/target/marker" ]]
    resolve_into "$root/opt/resolve-test/relative/marker"
    [[ $REPLY == "$root/opt/resolve-test/target/marker" ]]
    resolve_into "$root/opt/resolve-test/deep-link/../marker"
    [[ $REPLY == "$root/opt/resolve-test/target/marker" ]]
    resolve_into "$root/opt/resolve-test/space link/file with spaces"
    [[ $REPLY == "$root/opt/resolve-test/space target/file with spaces" ]]
    (cd "$root/opt/resolve-test"; resolve_into ./relative/marker
     [[ $REPLY == "$root/opt/resolve-test/target/marker" ]])
    resolve_into "$root/../../etc/native-test"
    [[ $REPLY == "$root/etc/native-test" ]]
    # Exactly forty symlinks are valid; forty-one and cycles must fail.
    for ((link = 40; link >= 1; --link)); do
        target=chain-$((link + 1)); [[ $link != 40 ]] || target=target/marker
        ln -s "$target" "$root/opt/resolve-test/chain-$link"
    done
    resolve_into "$root/opt/resolve-test/chain-1"
    [[ $REPLY == "$root/opt/resolve-test/target/marker" ]]
    ln -s chain-1 "$root/opt/resolve-test/chain-0"
    ln -s cycle "$root/opt/resolve-test/cycle"
    if (resolve_into "$root/opt/resolve-test/chain-0") >/dev/null 2>&1; then
        echo 'resolver accepted 41 symlinks' >&2; exit 1
    fi
    if (resolve_into "$root/opt/resolve-test/cycle") >/dev/null 2>&1; then
        echo 'resolver accepted symlink cycle' >&2; exit 1
    fi
    echo 'component-wise shell resolver checks OK'
    (
        unset TZ
        timezone_calls=$work/timezone-calls
        : > "$timezone_calls"
        test_timezone=Europe/Prague
        function /system/bin/getprop {
            [[ $# == 1 && $1 == persist.sys.timezone ]] || return 2
            printf 'getprop\n' >> "$timezone_calls"
            printf '%s\n' "$test_timezone"
        }
        rm -f "$N/timezone.cache"
        native_timezone
        [[ $REPLY == Europe/Prague && $(wc -l < "$timezone_calls") == 1 ]]
        mapfile -t timezone_cache < "$N/timezone.cache"
        [[ ${#timezone_cache[@]} == 2 && ${timezone_cache[0]} =~ ^[0-9]+$ && ${timezone_cache[1]} == Europe/Prague ]]
        test_timezone=Europe/Paris
        # A fresh cache also works in a newly sourced launcher process.
        cached_timezone=$(source <(sed '/^command=/,$d' /repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native)
                          native_timezone; printf '%s' "$REPLY")
        [[ $cached_timezone == Europe/Prague && $(wc -l < "$timezone_calls") == 1 ]]
        printf '%s\n%s\n' "$((EPOCHSECONDS - 3601))" Europe/Prague > "$N/timezone.cache"
        native_timezone
        [[ $REPLY == Europe/Paris && $(wc -l < "$timezone_calls") == 2 ]]
        TZ=Pacific/Honolulu native_timezone
        [[ $REPLY == Pacific/Honolulu && $(wc -l < "$timezone_calls") == 2 ]]
        mapfile -t timezone_cache < "$N/timezone.cache"
        [[ ${timezone_cache[1]} == Europe/Paris ]]
        printf 'not-an-epoch\nEurope/Prague\n' > "$N/timezone.cache"
        native_timezone
        [[ $REPLY == Europe/Paris && $(wc -l < "$timezone_calls") == 3 ]]
        test_timezone=
        rm -f "$N/timezone.cache"
        native_timezone
        [[ -z $REPLY && $(wc -l < "$timezone_calls") == 4 ]]
        mapfile -t timezone_cache < "$N/timezone.cache"
        [[ ${#timezone_cache[@]} == 2 && -z ${timezone_cache[1]} ]]
        test_timezone=Europe/Prague
        native_timezone
        [[ -z $REPLY && $(wc -l < "$timezone_calls") == 4 ]]
        echo 'hourly timezone cache and explicit TZ checks OK'
    )
    mkdir -p "$root/opt/dummy"
    cp "$root/usr/bin/probe" "$root/opt/dummy/brave"
    cp "$root/usr/bin/probe" "$root/opt/dummy/chrome_crashpad_handler"
    printf 'ICU fixture\n' > "$root/opt/dummy/icudtl.dat"
    before=$(sha256sum "$root/opt/dummy/brave")
    mirror_app /opt/dummy brave > "$work/app-command"
    grep -q -- '--no-sandbox --disable-dev-shm-usage --password-store=basic' "$work/app-command"
    [[ -L $N/apps/dummy/icudtl.dat && ! -L $N/apps/dummy/brave && ! -L $N/apps/dummy/chrome_crashpad_handler ]]
    [[ $(patchelf --print-interpreter "$N/apps/dummy/brave") == "$PLIB/ld.so" ]]
    [[ $(sha256sum "$root/opt/dummy/brave") == "$before" ]]
    mirror_app /opt/dummy brave >/dev/null
    (LANG=xx_YY.UTF-8 run_native "$N/apps/dummy/brave" app)
    # Source executables keep adjacent data and their real self path without any mirror.
    (LANG=xx_YY.UTF-8 run_native /opt/dummy/brave app)
    export TERMUX_VERSION=0.118 TERMUX__TEST='value with spaces'
    (run_native /usr/bin/android-marker android)
    mkdir -p "$root/etc/profile.d"
    printf 'export GALLIUM_DRIVER=virpipe LD_LIBRARY_PATH=/opt/native-gpu/lib\n' > "$root/etc/profile.d/droiddesk-gui.sh"
    (export CUSTOM='value with spaces' MESA_TEST=mesa LC_TIME=xx_YY.UTF-8 DISPLAY=:7
     export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/custom-bus
     export DD_NATIVE_PASS='CUSTOM PREFIX TERMUX_VERSION LD_PRELOAD LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS'
     run_native /usr/bin/probe desktop)

    # Real ELF RUNPATH/RPATH fixtures, including a library found only through
    # another library RUNPATH. The loader reads these paths past the hook.
    libdir=$root/usr/lib/aarch64-linux-gnu
    mkdir -p "$libdir/pulseaudio" "$libdir/legacy"
    printf 'int dd_pulse_value(void) { return 73; }\n' | gcc -shared -fPIC -x c - -o "$libdir/pulseaudio/libdd-pulse.so"
    printf 'extern int dd_pulse_value(void); int dd_runpath_value(void) { return dd_pulse_value(); }\n' |
        gcc -shared -fPIC -x c - -L"$libdir/pulseaudio" -ldd-pulse \
            -Wl,-rpath,/usr/lib/aarch64-linux-gnu/pulseaudio -o "$libdir/libdd-runpath.so"
    printf 'int dd_legacy_value = 17;\n' | gcc -shared -fPIC -x c - \
        -Wl,--disable-new-dtags,-rpath,/usr/lib/aarch64-linux-gnu/legacy:/missing-dd-lib -o "$libdir/legacy/libdd-legacy.so"
    setup_libraries
    [[ $(< "$N/libpaths") == *"$libdir/pulseaudio"* && $(< "$N/libpaths") == *"$libdir/legacy"* ]]
    [[ $(< "$N/libpaths") != *missing-dd-lib* ]]
    (run_native /usr/bin/probe runpath)
    # Changes below usr/lib must invalidate the cache too.
    mkdir -p "$libdir/new-path"
    touch -d '2 seconds ago' "$N/lib/.links"
    printf 'int dd_subdir_value;\n' | gcc -shared -fPIC -x c - \
        -Wl,-rpath,/usr/lib/aarch64-linux-gnu/new-path -o "$libdir/pulseaudio/libdd-subdir.so"
    setup_libraries
    [[ $(< "$N/libpaths") == *"$libdir/new-path"* ]]
    echo 'recursive ELF RPATH/RUNPATH cache and dependency loading checks OK'

    # A changed directory scans once; warm successful launches never invoke Python.
    cp /usr/bin/true "$root/usr/bin/true"
    (
        python_calls=$work/hot-path-python-calls
        : > "$python_calls"
        python3() { printf 'python3\n' >> "$python_calls"; command python3 "$@"; }
        ln -s /opt/native-test.so "$root/usr/lib/aarch64-linux-gnu/libhot-path.so"
        rm -f "$N/lib/.links"
        (try_native /usr/bin/true)
        [[ $(readlink "$N/lib/libhot-path.so") == "$root/opt/native-test.so" ]]
        [[ $(wc -l < "$python_calls") == 1 ]]
        : > "$python_calls"
        (run_native /usr/bin/true)
        (run_native /usr/bin/true)
        [[ ! -s $python_calls ]]
        start_us=${EPOCHREALTIME/./}
        for ((iteration = 0; iteration < 20; ++iteration)); do (try_native /usr/bin/true); done
        elapsed_us=$((${EPOCHREALTIME/./} - start_us))
        average_us=$((elapsed_us / 20))
        printf '20x try /usr/bin/true average: %d.%03d ms (informational)\n' \
            "$((average_us / 1000))" "$((average_us % 1000))"
        [[ ! -s $python_calls ]]
        echo 'Python-free warm native launch and one-scan refresh checks OK'
    )

    expect_proot() {
        if (try_native "$@"); then echo 'try unexpectedly ran a command' >&2; exit 1
        else [[ $? == 3 ]]; fi
    }
    [[ $(native_switch status) == *'připravený a zapnutý'* ]]
    native_switch off
    [[ -f $N/off ]]
    expect_proot probe --secret-argument
    native_switch on
    [[ ! -e $N/off ]]
    DD_NATIVE=0 expect_proot probe
    for cmd in apt apt-get apt-cache aptitude dpkg dpkg-reconfigure debconf sudo su passwd useradd usermod groupadd chroot mount umount systemctl service update-alternatives proot-distro droiddesk-session-bus; do
        expect_proot "$cmd"
        expect_proot /usr/bin/"$cmd"
        expect_proot bash -lc "$cmd --secret-argument"
    done
    # CLI compatibility runtimes must request proot without running natively,
    # including local symlink wrappers whose resolved basename is just probe.
    mkdir -p "$root/usr/local/bin"
    printf '#!/bin/sh\nexec "$@"\n' > "$root/usr/local/bin/gpu-run"
    chmod +x "$root/usr/local/bin/gpu-run"
    wine_feedback=$work/wine-result
    for cmd in wine wine64 wine-stable wineserver wineboot winecfg wineconsole winetricks hangover hangover-wine box64 box86 fex fex-shell FEXInterpreter FEXLoader; do
        ln -s /usr/bin/probe "$root/usr/local/bin/$cmd"
        for path in "$cmd" "/usr/local/bin/$cmd"; do
            DD_NATIVE_GUI=0 expect_proot "$path"
            : > "$wine_feedback"
            DD_NATIVE_GUI=0 DD_NATIVE_RESULT=$wine_feedback expect_proot env -u MESA_X TEST=1 /usr/local/bin/gpu-run "$path"
            [[ $(< "$wine_feedback") == proot ]]
        done
        grep -Fqx "proot proot-only $cmd" "$T/droiddesk-native.log"
    done
    [[ ! -s $N/proot-list ]]
    echo 'Wine/Hangover/Box/FEX basename and wrapped CLI preflight checks OK'
    expect_proot bash -c 'exec /usr/bin/apt-get update'
    expect_proot bash -lc $'\napt-get update'
    expect_proot missing-native-command
    expect_proot /usr/bin/../bin/missing-native-command
    expect_proot "$PREFIX/bin/env"
    ln -s /usr/bin/probe "$root/usr/bin/native-link"
    [[ $(source_path native-link) == "$root/usr/bin/probe" ]]
    printf 'export DD_PROFILE=1 PATH="$HOME/.local/bin:$PATH"\n' > "$root/etc/profile"
    printf 'export DD_USER_PROFILE=1\n' > "$root/root/.profile"
    (try_native native-link context)
    ln -s /usr/bin/probe "$root/root/relative-probe"
    (try_native ./relative-probe context)
    [[ $(native_target /root/apps/test/AppRun) == "$root/root/apps/test/AppRun" ]]
    [[ $(native_target /home/test/home-probe) == "$root/home/test/home-probe" ]]
    [[ $(native_target "$root/root/apps/test/AppRun") == "$root/root/apps/test/AppRun" ]]
    [[ $(native_program env -u MESA_X GALLIUM_DRIVER=llvmpipe /root/apps/test/AppRun) == AppRun ]]
    (try_native /root/apps/test/AppRun context)
    mv "$N/stamp" "$N/stamp.saved"
    expect_proot probe
    mv "$N/stamp.saved" "$N/stamp"
    mv "$PLIB/libc.so.6" "$PLIB/libc.saved"
    expect_proot probe
    mv "$PLIB/libc.saved" "$PLIB/libc.so.6"
    ! grep -q -- '--secret-argument' "$T/droiddesk-native.log"
    grep -q '^native ready probe$' "$T/droiddesk-native.log"
    grep -q '^proot disabled probe$' "$T/droiddesk-native.log"
    grep -q '^proot not-ready probe$' "$T/droiddesk-native.log"
    head -c 70000 /dev/zero > "$T/droiddesk-native.log"
    expect_proot apt
    [[ $(wc -c < "$T/droiddesk-native.log") -lt 66000 ]]
    # Rotation keeps recent readable decisions and discards the old large prefix.
    { printf 'old-prefix\n'; head -c 70000 /dev/zero | tr '\0' x
      printf '\nrecent-decision\n'; } > "$T/droiddesk-native.log"
    expect_proot apt
    [[ $(wc -c < "$T/droiddesk-native.log") -lt 66000 ]]
    ! grep -q '^old-prefix$' "$T/droiddesk-native.log"
    grep -q '^recent-decision$' "$T/droiddesk-native.log"
    grep -q '^proot proot-only apt$' "$T/droiddesk-native.log"
    echo 'native decision and desktop environment checks OK'

    # Extract the actual generated launchers, not hand-maintained copies of their logic.
    python3 - "$root" "$PREFIX" "$T" <<'PY'
import pathlib, re, sys, textwrap
root, prefix, tmp = map(pathlib.Path, sys.argv[1:])
text = pathlib.Path('/repo/app/android/app/src/main/kotlin/com/orailnoor/droiddesk/runtime/LinuxRuntime.kt').read_text()
body = text.split("cat > /usr/local/bin/electron-run <<'DROIDDESK_ELECTRON_RUN'\n", 1)[1]
body = body.split('\n            DROIDDESK_ELECTRON_RUN', 1)[0]
electron = root / 'usr/local/bin/electron-run'
electron.parent.mkdir(parents=True, exist_ok=True)
electron.write_text(textwrap.dedent(body).replace("${'$'}", '$') + '\n')
electron.chmod(0o755)
body = text.split('val runLauncher =', 1)[1].split('"""', 2)[1]
body = textwrap.dedent(body).lstrip('\n').replace("${'$'}", '$')
body = body.replace('${tmpDir.absolutePath}', str(tmp)).replace('${binDir.absolutePath}', str(prefix / 'bin'))
body = re.sub(r'\$\{File\(binDir, "([^"]+)"\)\.absolutePath\}', lambda m: str(prefix / 'bin' / m[1]), body)
body = re.sub(r'\$\{File\(prefixDir, "([^"]+)"\)\.absolutePath\}', lambda m: str(prefix / m[1]), body)
(prefix / 'bin/debian-run').write_text(body + '\n')
# Later checks replace debian-run with stubs; the protocol check restores this copy.
(prefix / 'debian-run.generated').write_text(body + '\n')
body = text.split('val tmuxLauncher =', 1)[1].split('"""', 2)[1]
body = textwrap.dedent(body).lstrip('\n').replace("${'$'}", '$')
body = body.replace('${tmpDir.absolutePath}', str(tmp)).replace('${binDir.absolutePath}', str(prefix / 'bin'))
body = re.sub(r'\$\{File\(binDir, "([^"]+)"\)\.absolutePath\}', lambda m: str(prefix / 'bin' / m[1]), body)
body = re.sub(r'\$\{File\(prefixDir, "([^"]+)"\)\.absolutePath\}', lambda m: str(prefix / m[1]), body)
(prefix / 'bin/debian-tmux').write_text(body + '\n')
(prefix / 'bin/debian-tmux').chmod(0o755)
# Isolate the actual host body and redirect its passwd access to a test fixture.
host = body.split(" bash -c '\n", 1)[1].split("' droiddesk-tmux-host", 1)[0]
assert "'" not in host, 'host body must remain safe inside single-quoted bash -c'
host = host.replace('/etc/passwd', '"$TEST_PASSWD"')
(tmp / 'tmux-host.sh').write_text(host + '\n')
# Validate every generated shell body touched by the native GUI changes.
for tag in ('ELECTRON_RUN', 'GPU_RUN', 'MENU_SYNC', 'DBUS_ENV'):
    body = text.split("<<'DROIDDESK_" + tag + "'\n", 1)[1].split('\n            DROIDDESK_' + tag, 1)[0]
    body = textwrap.dedent(body).replace("${'$'}", '$') + '\n'
    import subprocess
    subprocess.run(['bash', '-n'], input=body, text=True, check=True)
    if tag == 'MENU_SYNC':
        detect = body[body.index('ELECTRON_NAMES='):body.index('# Custom URL schemes')]
        subprocess.run(['bash', '-c', detect + '\nis_electron "env -u MESA_X GALLIUM_DRIVER=llvmpipe /usr/bin/brave-browser"'], check=True)
# proot keeps the XFCE bus; Debian's profile switches to Debian's bus (keyring) while it runs.
# Native launches always get Debian's bus, never the Termux Secret Service (keyring incident).
assert '--env DBUS_SESSION_BUS_ADDRESS="unix:path=/tmp/dbus-session"' in (prefix / 'bin/debian-run').read_text()
assert "if [ -S /tmp/dbus-debian ]; then export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-debian; fi" in text
assert "'DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-debian'" in pathlib.Path('/repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native').read_text()
subprocess.run(['bash', '-n', str(electron)], check=True)
subprocess.run(['bash', '-n', str(prefix / 'bin/debian-run')], check=True)
assert 'Exec=env DD_NATIVE_GUI=1' in text
assert '99droiddesk-menu-sync' in text
PY
    (LANG=xx_YY.UTF-8 run_native /usr/local/bin/electron-run /opt/dummy/brave app)
    (LANG=xx_YY.UTF-8 run_native /usr/local/bin/electron-run env -u MESA_X GALLIUM_DRIVER=llvmpipe /opt/dummy/brave app)
    (unset DBUS_SESSION_BUS_ADDRESS DD_NATIVE_PASS; run_native /usr/bin/probe bus)
    [[ ! -L $root/opt/dummy/brave ]]

    # Resolve env options and nested wrappers without changing the executed argv.
    for args in '-i' '-u MESA_X' '--unset=MESA_X' '--unset MESA_X' '-0' '--'; do
        read -r -a env_args <<< "$args"
        [[ $(native_program env "${env_args[@]}" GALLIUM_DRIVER=llvmpipe gpu-run /opt/dummy/brave) == brave ]]
    done
    [[ $(native_program nice -n 5 setsid -f stdbuf -o L env -u MESA_X /opt/dummy/brave) == brave ]]
    if native_program env -u; then echo 'accepted incomplete env option' >&2; exit 1; fi
    (export MESA_X=remove_me
     run_native env -u MESA_X GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 /bin/bash -c '
         [[ ! -v MESA_X && $GALLIUM_DRIVER == llvmpipe && $LIBGL_ALWAYS_SOFTWARE == 1 ]]
     ')

    # Electron must keep the real executable as the process image, including behind wrappers.
    (
        appdir=$root/opt/electron-fixture
        mirror=$N/apps/electron-fixture/brave
        mkdir -p "$appdir/resources"
        cp "$root/usr/bin/probe" "$appdir/brave"
        printf 'asar fixture\n' > "$appdir/resources/app.asar"
        printf 'ICU fixture\n' > "$appdir/icudtl.dat"
        ln -s /opt/electron-fixture/brave "$root/usr/bin/electron-alias"
        original=$(sha256sum "$appdir/brave")
        eval "$(declare -f mirror_app | sed '1s/mirror_app/real_mirror_app/')"
        mirror_app() { printf 'build\n' >> "$work/electron-builds"; real_mirror_app "$@"; }
        # Retain the real launch, while recording the exact argv handed to it.
        eval "$(declare -f native_launch | sed '1s/native_launch/real_native_launch/')"
        native_launch() { printf '%s\0' "$@" > "$work/electron-argv"; real_native_launch "$@"; }
        (LANG=xx_YY.UTF-8 try_native electron-alias context)
        mapfile -d '' -t actual < "$work/electron-argv"
        [[ ${#actual[@]} == 2 && ${actual[0]} == "$mirror" && ${actual[1]} == context ]]
        [[ -f $mirror && ! -L $mirror && -L ${mirror%/*}/resources ]]
        [[ $(patchelf --print-interpreter "$mirror") == "$PLIB/ld.so" ]]
        patchelf --print-needed "$mirror" | grep -Fxq "$N/lib/pathhook.so"
        [[ $(sha256sum "$appdir/brave") == "$original" ]]
        # Adjacent data, /proc/self/exe re-exec and electron-run flags still work.
        (LANG=xx_YY.UTF-8 try_native /usr/local/bin/electron-run env -u MESA_X TEST='value with spaces' gpu-run electron-alias app)
        mapfile -d '' -t actual < "$work/electron-argv"
        expected=(/usr/local/bin/electron-run env -u MESA_X 'TEST=value with spaces' gpu-run "$mirror" app)
        [[ ${#actual[@]} == ${#expected[@]} ]]
        for index in "${!expected[@]}"; do [[ ${actual[index]} == "${expected[index]}" ]]; done
        # The warm mirror path must neither inspect ELF data nor patch anything.
        (electron_elf() { return 99; }; mirror_app() { return 99; }; python3() { return 99; }
         LANG=xx_YY.UTF-8 try_native electron-alias context)
        [[ $(wc -l < "$work/electron-builds") == 1 ]]
        old_stamp=$(< "${mirror%/*}/.electron-stamp")
        touch -d '2 seconds hence' "$appdir/brave"
        (LANG=xx_YY.UTF-8 try_native electron-alias context)
        [[ $(< "${mirror%/*}/.electron-stamp") != "$old_stamp" && $(wc -l < "$work/electron-builds") == 2 ]]
        old_stamp=$(< "${mirror%/*}/.electron-stamp")
        cp -p "$appdir/brave" "$work/electron-time"
        printf '\n' >> "$appdir/brave"
        touch -r "$work/electron-time" "$appdir/brave"
        (LANG=xx_YY.UTF-8 try_native electron-alias context)
        [[ $(< "${mirror%/*}/.electron-stamp") != "$old_stamp" && $(wc -l < "$work/electron-builds") == 3 ]]
        rm "$appdir/resources/app.asar"
        mkdir "$appdir/resources/app"
        electron_elf "$appdir/brave"
        # A string outside .rodata must not classify an ordinary ELF as Electron.
        cp "$root/usr/bin/probe" "$appdir/plain"
        printf 'electron\n' >> "$appdir/plain"
        mkdir "$root/opt/signature-only"
        mv "$appdir/plain" "$root/opt/signature-only/plain"
        if electron_elf "$root/opt/signature-only/plain"; then echo 'matched outside .rodata' >&2; exit 1; fi
        printf 'const char signature[] = "electron"; int main(void) { return 0; }\n' |
            gcc -x c - -o "$root/opt/signature-only/electron"
        electron_elf "$root/opt/signature-only/electron"
        # Missing Termux patchelf is a preflight decision, never a failed native run.
        (
            command() {
                if [[ ${1:-} == -v && ${2:-} == patchelf ]]; then return 1; fi
                builtin command "$@"
            }
            native_launch() { echo 'unexpected Electron launch' >&2; return 99; }
            feedback=$work/electron-result
            : > "$feedback"
            DD_NATIVE_GUI=0 DD_NATIVE_RESULT=$feedback expect_proot electron-alias 2> "$work/electron-hint"
            [[ $(< "$feedback") == proot && $(wc -l < "$work/electron-hint") == 1 ]]
            grep -Fq 'pkg install patchelf' "$work/electron-hint"
            grep -Fxq 'proot electron-needs-patchelf brave' "$T/droiddesk-native.log"
            : > "$feedback"
            DD_NATIVE_GUI=1 DD_NATIVE_RESULT=$feedback expect_proot electron-alias </dev/null > "$work/electron-gui" 2>&1
            [[ $(< "$feedback") == proot && ! -s $work/electron-gui ]]
            # First use without patchelf must behave the same as a cached mirror.
            rm "${mirror%/*}/.electron-stamp"
            DD_NATIVE_GUI=0 expect_proot electron-alias 2> "$work/electron-hint"
            [[ ! -f ${mirror%/*}/.electron-stamp && ! -s $N/proot-list ]]
        )
        (mirror_app() { return 1; }; expect_proot electron-alias)
        grep -Fxq 'proot electron-mirror brave' "$T/droiddesk-native.log"
        echo 'automatic Electron mirror, refresh, argv and patchelf fallback checks OK'
    )

    # Learning is per resolved program, with generic wrappers peeled off.
    mkdir -p "$root/usr/local/bin"
    printf '#!/bin/sh\nprintf "start\\n" >> "$HOME/fast-starts"\nfor arg in "$@"; do :; done\nexit "${arg:-23}"\n' > "$root/usr/bin/fast-app"
    printf '#!/bin/sh\nexit 0\n' > "$root/usr/bin/ok-app"
    printf '#!/bin/sh\nkill -SEGV "$$"\n' > "$root/usr/bin/signal-app"
    printf '#!/bin/sh\nsleep 6\nexit 23\n' > "$root/usr/bin/slow-app"
    cp /usr/bin/sleep "$root/usr/bin/sleep"
    printf '#!/bin/sh\nexec "$@"\n' > "$root/usr/local/bin/gpu-run"
    chmod +x "$root/usr/bin/"{fast-app,ok-app,signal-app,slow-app} "$root/usr/local/bin/gpu-run"
    ln -s /usr/bin/fast-app "$root/usr/bin/fast-alias"
    feedback=$work/native-result
    : > "$feedback"
    DD_NATIVE_RESULT=$feedback expect_proot apt
    [[ $(cat "$feedback") == proot ]]
    : > "$feedback"
    # A normal non-GUI command returning 3 does not request fallback or enter the list.
    status=0
    (DD_NATIVE_RESULT=$feedback try_native fast-app 3) </dev/null >"$work/terminal.log" 2>&1 || status=$?
    [[ $status == 3 && ! -s $feedback && ! -s $N/proot-list ]]
    # A successful wrapper can hide a fatal child exit. Terminal runs stay single-shot.
    printf '#!/bin/sh\nprintf "Trace/breakpoint trap\\n" >&2\nexit 0\n' > "$root/usr/bin/marker-app"
    chmod +x "$root/usr/bin/marker-app"
    (DD_NATIVE_RESULT=$feedback try_native marker-app) </dev/null >"$work/marker-terminal.log" 2>&1
    [[ ! -s $feedback ]] && ! grep -Fxq marker-app "$N/proot-list"
    printf '#!/bin/bash\nprintf "proot\\n" >> "$TEST_RESTART_LOG"\nexit 17\n' > "$PREFIX/bin/debian-run"
    chmod +x "$PREFIX/bin/debian-run"
    export TEST_RESTART_LOG=$work/marker-restarts
    status=0
    printf 'previous stderr\n' > "$work/marker-gui.log"
    (unset DD_NATIVE_RESULT; DD_NATIVE_GUI=1 try_native marker-app) </dev/null >>"$work/marker-gui.log" 2>&1 || status=$?
    [[ $status == 17 && $(cat "$TEST_RESTART_LOG") == proot ]]
    grep -Fxq marker-app "$N/proot-list"
    grep -q 'Trace/breakpoint trap' "$work/marker-gui.log"
    grep -q '^previous stderr$' "$work/marker-gui.log"
    [[ -z $(find "$T" -name 'droiddesk-native-stderr.*' -print -quit) ]]
    native_reset marker-app
    # The same exit code for an opted-in GUI launch requests one restart and is learned.
    status=0
    (DD_NATIVE_GUI=1 DD_NATIVE_RESULT=$feedback try_native /usr/local/bin/electron-run env TEST=1 gpu-run fast-alias 3) </dev/null >"$work/gui.log" 2>&1 || status=$?
    [[ $status == 3 && $(cat "$feedback") == proot ]]
    grep -aq '^proot quick-exit-3 fast-app$' "$T/droiddesk-native.log"
    grep -Fxq fast-app "$N/proot-list"
    ! grep -Eq '^(env|gpu-run|electron-run|bash|fast-alias)$' "$N/proot-list"
    [[ $(wc -l < "$root/root/fast-starts") == 2 ]]
    : > "$feedback"
    DD_NATIVE_RESULT=$feedback expect_proot fast-alias
    [[ $(cat "$feedback") == proot && $(wc -l < "$root/root/fast-starts") == 2 ]]
    [[ $(native_switch status) == *fast-app* ]]
    DD_NATIVE_GUI=1 expect_proot bash -lc 'echo no-global-shell-fallback'
    ln -s /usr/bin/bash "$root/usr/bin/shell-alias"
    DD_NATIVE_GUI=1 expect_proot shell-alias -lc 'exit 9'
    ! grep -Fxq bash "$N/proot-list"
    : > "$feedback"
    ln -s /opt/native-test.so "$root/usr/lib/aarch64-linux-gnu/libnew-app.so"
    (DD_NATIVE_GUI=1 DD_NATIVE_RESULT=$feedback try_native ok-app) </dev/null >"$work/ok.log" 2>&1
    [[ ! -s $feedback ]] && ! grep -Fxq ok-app "$N/proot-list"
    [[ $(readlink "$N/lib/libnew-app.so") == "$root/opt/native-test.so" ]]
    ulimit -c 0
    status=0
    (DD_NATIVE_GUI=1 DD_NATIVE_RESULT=$feedback try_native signal-app) </dev/null >"$work/signal.log" 2>&1 || status=$?
    [[ $status == 3 && $(cat "$feedback") == proot ]]
    grep -Fxq signal-app "$N/proot-list"
    : > "$feedback"
    status=0
    (DD_NATIVE_GUI=1 DD_NATIVE_RESULT=$feedback try_native slow-app) </dev/null >"$work/slow.log" 2>&1 || status=$?
    [[ $status == 23 && ! -s $feedback ]] && ! grep -Fxq slow-app "$N/proot-list"
    native_reset fast-app
    ! grep -Fxq fast-app "$N/proot-list"
    grep -Fxq signal-app "$N/proot-list"
    proot_list_update add parallel-a & pid_a=$!
    proot_list_update add parallel-b & pid_b=$!
    wait "$pid_a"; wait "$pid_b"
    grep -Fxq parallel-a "$N/proot-list"
    grep -Fxq parallel-b "$N/proot-list"
    native_reset --all
    [[ ! -s $N/proot-list ]]
    native_learn tmux
    native_learn tmux
    [[ $(grep -Fxc tmux "$N/proot-list") == 1 ]]
    for invalid in '' /usr/bin/tmux '../tmux' '--all'; do
        if (native_learn "$invalid") >/dev/null 2>&1; then
            echo "learn accepted invalid name: $invalid" >&2; exit 1
        fi
    done
    if (native_learn tmux extra) >/dev/null 2>&1; then
        echo 'learn accepted extra arguments' >&2; exit 1
    fi
    native_reset tmux
    launcher=/repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native
    PREFIX=$PREFIX bash "$launcher" learn tmux
    grep -Fxq tmux "$N/proot-list"
    PREFIX=$PREFIX bash "$launcher" reset tmux
    ! grep -Fxq tmux "$N/proot-list"
    python3 - "$root" "$T" "$feedback" <<'PY'
import os, pty, subprocess, sys
root, tmp, feedback = sys.argv[1:]
script = '''source <(sed '/^command=/,$d' /repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native)
R=$1 T=$2
native_launch() { return 3; }
DD_NATIVE_GUI=1 DD_NATIVE_RESULT=$3 try_native fast-app'''
master, slave = pty.openpty()
try:
    result = subprocess.run(['bash', '-c', script, 'test-tty', root, tmp, feedback],
                            stdin=slave, stdout=slave, stderr=slave, timeout=15)
    assert result.returncode == 3, result.returncode
    assert not os.path.getsize(feedback)
finally:
    os.close(slave)
    os.close(master)
PY
    [[ ! -s $N/proot-list && ! -s $feedback ]]
    echo 'native learning/reset and original Electron binary checks OK'

    # The generated caller uses the side channel, never a program exit code, to retry.
    ln -s /bin/bash "$PREFIX/bin/bash"
    cp "$PREFIX/debian-run.generated" "$PREFIX/bin/debian-run"
    cat > "$PREFIX/bin/droiddesk-glibc-native" <<'SH'
#!/bin/bash
printf 'native\n' >> "$TEST_LAUNCH_LOG"
if [[ ${TEST_REAL_NATIVE_PREFLIGHT:-0} == 1 ]]; then
    source <(sed '/^command=/,$d' /repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native)
    R=$TEST_ROOTFS T=$TMPDIR
    try_native "${@:2}"
    exit $?
fi
if [[ ${TEST_NATIVE_FALLBACK:-0} == 1 ]]; then printf 'proot\n' > "$DD_NATIVE_RESULT"; fi
exit "$TEST_NATIVE_STATUS"
SH
    cat > "$PREFIX/bin/proot-distro" <<'SH'
#!/bin/bash
printf 'proot\n' >> "$TEST_LAUNCH_LOG"
exit 17
SH
    chmod +x "$PREFIX/bin/droiddesk-glibc-native" "$PREFIX/bin/proot-distro"
    export TEST_LAUNCH_LOG=$work/launches
    : > "$TEST_LAUNCH_LOG"
    status=0
    TEST_NATIVE_STATUS=3 TEST_NATIVE_FALLBACK=0 bash "$PREFIX/bin/debian-run" fast-app || status=$?
    [[ $status == 3 && $(cat "$TEST_LAUNCH_LOG") == native ]]
    : > "$TEST_LAUNCH_LOG"
    status=0
    TEST_NATIVE_STATUS=3 TEST_NATIVE_FALLBACK=1 bash "$PREFIX/bin/debian-run" fast-app || status=$?
    [[ $status == 17 && $(grep -c '^native$' "$TEST_LAUNCH_LOG") == 1 && $(grep -c '^proot$' "$TEST_LAUNCH_LOG") == 1 ]]
    [[ -z $(find "$T" -name 'droiddesk-native-result.*' -print -quit) ]]
    # The actual generated launcher must honor the real preflight for CLI commands.
    for cmd in wine hangover-wine; do
        : > "$TEST_LAUNCH_LOG"
        status=0
        TEST_REAL_NATIVE_PREFLIGHT=1 TEST_ROOTFS=$root bash "$PREFIX/bin/debian-run" env -u MESA_X TEST=1 /usr/local/bin/gpu-run "/usr/local/bin/$cmd" || status=$?
        [[ $status == 17 && $(cat "$TEST_LAUNCH_LOG") == $'native\nproot' ]]
    done
    [[ -z $(find "$T" -name 'droiddesk-native-result.*' -print -quit) ]]
    echo 'unambiguous single-restart launcher protocol checks OK'

    # Exercise the generated tmux wrapper with the native timeout side channel.
    bash -n "$PREFIX/bin/debian-tmux"
    cat > "$PREFIX/bin/debian-run" <<'SH'
#!/bin/bash
[[ $4 == droiddesk-tmux-host ]] && exit 0
printf 'attach %s\n' "${DD_NATIVE:-1}" >> "$TEST_LAUNCH_LOG"
if [[ ${DD_NATIVE:-1} != 0 ]]; then
    [[ $TEST_TMUX_MARKER == 1 ]] && printf 'native\n' > "$7"
    exit 1
fi
exit "$TEST_TMUX_PROOT_STATUS"
SH
    cat > "$PREFIX/bin/droiddesk-glibc-native" <<'SH'
#!/bin/bash
[[ $1 == learn && $2 == tmux && $# == 2 ]] || exit 2
printf 'learn tmux\n' >> "$TEST_LAUNCH_LOG"
printf 'tmux\n' > "$PREFIX/opt/glibc-native/proot-list"
SH
    chmod 755 "$PREFIX/bin/debian-run" "$PREFIX/bin/droiddesk-glibc-native"
    export TEST_TMUX_MARKER=1 TEST_TMUX_PROOT_STATUS=0
    : > "$N/proot-list"; : > "$TEST_LAUNCH_LOG"
    bash "$PREFIX/bin/debian-tmux" test-session 'argument with spaces' </dev/null
    [[ $(cat "$TEST_LAUNCH_LOG") == $'attach 1\nlearn tmux\nattach 0' ]]
    [[ -z $(find "$T" -name 'droiddesk-tmux-result.*' -print -quit) ]]
    # Learned tmux starts in proot, and even a failed retry cannot recurse.
    : > "$TEST_LAUNCH_LOG"
    status=0
    TEST_TMUX_PROOT_STATUS=17 bash "$PREFIX/bin/debian-tmux" test-session </dev/null || status=$?
    [[ $status == 17 && $(cat "$TEST_LAUNCH_LOG") == 'attach 0' ]]
    # An ordinary attach error never learns or retries without the native marker.
    : > "$N/proot-list"; : > "$TEST_LAUNCH_LOG"
    status=0
    TEST_TMUX_MARKER=0 bash "$PREFIX/bin/debian-tmux" test-session </dev/null || status=$?
    [[ $status == 1 && $(cat "$TEST_LAUNCH_LOG") == 'attach 1' && ! -s $N/proot-list ]]
    [[ -z $(find "$T" -name 'droiddesk-tmux-result.*' -print -quit) ]]
    echo 'generated tmux single-retry and learning checks OK'

    # Run the real generated host body with fake identity/NSS and a private passwd.
    cat > "$T/tmux-host-test.sh" <<'SH'
#!/bin/bash
id() { [[ $1 == -u ]] || return 2; printf '%s\n' "$TEST_REAL_UID"; }
getent() { printf 'getent\n' >> "$TEST_GETENT_LOG"; return 2; }
sleep() { :; }
tmux() {
    local arg
    for arg in "$@"; do
        [[ $arg == has-session ]] && return 1
        [[ $arg == new-session ]] && return 0
    done
    return 2
}
source "$TEST_HOST_SCRIPT" host-session 42420 42420 android-user 'command with spaces'
SH
    export TEST_PASSWD=$T/tmux-passwd TEST_GETENT_LOG=$T/tmux-getent.log
    export TEST_HOST_SCRIPT=$T/tmux-host.sh
    host_home=$work/tmux-host-home
    mkdir -p "$host_home"
    bash -n "$TEST_HOST_SCRIPT"
    bash -n "$T/tmux-host-test.sh"
    printf 'existing:x:42420:42420:Existing:/:/bin/sh\n' > "$TEST_PASSWD"
    before=$(sha256sum "$TEST_PASSWD")
    : > "$TEST_GETENT_LOG"
    HOME=$host_home TEST_REAL_UID=42420 bash "$T/tmux-host-test.sh"
    [[ $(sha256sum "$TEST_PASSWD") == "$before" && ! -s $TEST_GETENT_LOG ]]
    ! grep -q 'server-access' "$host_home/.config/droiddesk/tmux.conf"
    # NSS misses the existing user under fake root; the file prevents duplicates.
    HOME=$host_home TEST_REAL_UID=0 bash "$T/tmux-host-test.sh"
    [[ $(sha256sum "$TEST_PASSWD") == "$before" && -s $TEST_GETENT_LOG ]]
    grep -Fxq 'server-access -a existing' "$host_home/.config/droiddesk/tmux.conf"
    # A missing proot entry is appended exactly once, even when NSS keeps missing.
    printf 'other:x:42421:42421:Other:/:/bin/sh\n' > "$TEST_PASSWD"
    HOME=$host_home TEST_REAL_UID=0 bash "$T/tmux-host-test.sh"
    [[ $(grep -c ':x:42420:' "$TEST_PASSWD") == 1 ]]
    grep -Fxq 'server-access -a aid_android-user' "$host_home/.config/droiddesk/tmux.conf"
    before=$(sha256sum "$TEST_PASSWD")
    HOME=$host_home TEST_REAL_UID=0 bash "$T/tmux-host-test.sh"
    [[ $(sha256sum "$TEST_PASSWD") == "$before" ]]
    echo 'generated tmux native passwd guard and proot duplicate checks OK'
)
# Exercise patcher failure and success without needing an Android device.
python3 - "$work" <<'PY'
import pathlib, struct, subprocess, sys
w = pathlib.Path(sys.argv[1])
patch = '/repo/app/android/app/src/main/assets/droiddesk/glibc-native/patch-glibc.py'
import runpy
patcher = runpy.run_path(patch)
blocked = {18,39,40,42,51,58,89,99,100,104,105,106,112,116,142,143,144,145,146,
           149,151,152,159,161,162,170,171,*range(180,198),217,218,219,224,225,
           234,235,236,237,238,239,244,262,263,264,265,266,272,273,288,289,290,
           292,293,294,425,426,427,428,429,430,431,432,433,442,443,447,449,450,
           451,453,454,455,456,457,458,459,460}
assert set(patcher['BLOCK']) == blocked
header = bytearray(64)
header[:6] = b'\x7fELF\x02\x01'
struct.pack_into('<H', header, 18, 183)
def svc(nr):
    return struct.pack('<II', 0xd2800008 | (nr << 5), 0xd4000001)
for kind in ('ld', 'libc'):
    src, dst = w / (kind + '.src'), w / (kind + '.dst')
    numbers = sorted(blocked) + [147, 202]
    src.write_bytes(header + b''.join(svc(nr) for nr in numbers) + (b'/etc/resolv.conf\0' if kind == 'libc' else b''))
    result = subprocess.run(['python3', patch, kind, str(src), str(dst)],
                            check=True, capture_output=True, text=True)
    patched = dst.read_bytes()
    for i, nr in enumerate(numbers):
        offset = 68 + i * 8
        expected = 0x928004a0 if nr in blocked else 0xd4000001
        assert struct.unpack_from('<I', patched, offset)[0] == expected, (kind, nr)
        if nr in blocked:
            assert f"{kind} {hex(offset)} {patcher['BLOCK'][nr]}" in result.stdout.splitlines()
    assert len(result.stdout.splitlines()) == len(blocked) + (kind == 'libc')
    # Both original required sites are mandatory, even with every extra blocked site.
    for omitted in ({99}, {293}, {99, 293}):
        src.write_bytes(header + b''.join(svc(nr) for nr in sorted(blocked - omitted)) +
                        (b'/etc/resolv.conf\0' if kind == 'libc' else b''))
        dst.unlink(missing_ok=True)
        assert subprocess.run(['python3', patch, kind, str(src), str(dst)], capture_output=True).returncode != 0
        assert not dst.exists(), 'failed patch must not create output'
    if kind == 'libc':
        src.write_bytes(header + svc(99) + svc(293))
        assert subprocess.run(['python3', patch, kind, str(src), str(dst)], capture_output=True).returncode != 0
        assert not dst.exists(), 'missing resolv.conf must fail'
# Intervening writes to x8 and syscalls outside the matching window stay untouched.
for code in (svc(272)[:4] + struct.pack('<I', 0xd2800008) + svc(272)[4:],
             svc(272)[:4] + struct.pack('<13I', *([0xd503201f] * 12 + [0xd4000001]))):
    data = bytearray(code)
    assert not patcher['patch_svc'](data, blocked) and bytes(data) == code
src, dst = w / 'libuv.src', w / 'libuv.dst'
src.write_bytes(header + struct.pack('<5I', 0xd2803520, 0x94000001, 0, 0, 0))
subprocess.run(['python3', patch, 'libuv', str(src), str(dst)], check=True)
assert struct.unpack_from('<I', dst.read_bytes(), 68)[0] == 0x92800000
# Exercise the embedded ELF parser without section headers or a compiler.
launcher = pathlib.Path('/repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native').read_text()
scanner = launcher.split("<<'PY'\n", 1)[1].split('\nPY', 1)[0]
ns = {'__name__': 'scanner_test'}
exec(scanner, ns)
elf = bytearray(1024)
elf[:6] = b'\x7fELF\x02\x01'
struct.pack_into('<Q', elf, 32, 64)
struct.pack_into('<HH', elf, 54, 56, 2)
struct.pack_into('<II6Q', elf, 64, 1, 0, 0, 0x400000, 0, len(elf), len(elf), 4096)
struct.pack_into('<II6Q', elf, 120, 2, 0, 256, 0x400100, 0, 80, 80, 8)
strings = b'/usr/lib/pulseaudio:$ORIGIN/ignored:relative\0/usr/lib/legacy\0'
elf[512:512 + len(strings)] = strings
for i, (tag, value) in enumerate(((5, 0x400200), (10, len(strings)), (29, 0),
                                  (15, strings.index(b'/usr/lib/legacy')), (0, 0))):
    struct.pack_into('<qQ', elf, 256 + i * 16, tag, value)
assert ns['elf_runpaths'](bytes(elf)) == ['/usr/lib/pulseaudio', '/usr/lib/legacy']
for size in (0, 4, 63, 127, 300):
    assert ns['elf_runpaths'](bytes(elf[:size])) == []
struct.pack_into('<Q', elf, 264, 0xffffffffffffffff)
assert ns['elf_runpaths'](bytes(elf)) == []
print('synthetic ELF dynamic-path checks OK')
print('synthetic seccomp patch checks OK')
PY
printf 'GLIBC_NATIVE_OK\n'
