#!/usr/bin/env bash
set -euo pipefail
apt-get update -qq
apt-get install -y -qq --no-install-recommends gcc libc6-dev patchelf python3
work=$(mktemp -d /data-glibc-native.XXXXXX)
trap 'rm -rf "$work"' EXIT
root=$work/rootfs
mkdir -p "$root"/{etc,var,usr/bin,root,opt/search/probe} "$work"/{tmp,plib} /system/bin
ln -s usr/bin "$root/bin"
printf 'mapped\n' > "$root/etc/native-test"
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
    touch "$N/stamp"
    mkdir -p "$root/usr/lib/locale/C.utf8"
    ln -s /opt/native-test.so "$root/usr/lib/aarch64-linux-gnu/libnative-alias.so"
    setup_data
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
     export DD_NATIVE_PASS='CUSTOM PREFIX TERMUX_VERSION LD_PRELOAD LD_LIBRARY_PATH'
     run_native /usr/bin/probe desktop)

    # Successful launches and a required library refresh must never invoke Python.
    cp /usr/bin/true "$root/usr/bin/true"
    (
        python_calls=$work/hot-path-python-calls
        : > "$python_calls"
        python3() { printf 'python3\n' >> "$python_calls"; return 99; }
        ln -s /opt/native-test.so "$root/usr/lib/aarch64-linux-gnu/libhot-path.so"
        rm -f "$N/lib/.links"
        (try_native /usr/bin/true)
        [[ $(readlink "$N/lib/libhot-path.so") == "$root/opt/native-test.so" ]]
        (run_native /usr/bin/true)
        # Repeat with an already refreshed mirror to exercise the normal fast path.
        (run_native /usr/bin/true)
        [[ ! -s $python_calls ]]
        start_us=${EPOCHREALTIME/./}
        for ((iteration = 0; iteration < 20; ++iteration)); do (try_native /usr/bin/true); done
        elapsed_us=$((${EPOCHREALTIME/./} - start_us))
        average_us=$((elapsed_us / 20))
        printf '20x try /usr/bin/true average: %d.%03d ms (informational)\n' \
            "$((average_us / 1000))" "$((average_us % 1000))"
        [[ ! -s $python_calls ]]
        echo 'Python-free successful native launch and library refresh checks OK'
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
    for cmd in apt apt-get apt-cache aptitude dpkg dpkg-reconfigure debconf sudo su passwd useradd usermod groupadd chroot mount umount systemctl service update-alternatives proot-distro; do
        expect_proot "$cmd"
        expect_proot /usr/bin/"$cmd"
        expect_proot bash -lc "$cmd --secret-argument"
    done
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
assert 'Exec=env DD_NATIVE_GUI=1' in text
assert '99droiddesk-menu-sync' in text
PY
    (LANG=xx_YY.UTF-8 run_native /usr/local/bin/electron-run /opt/dummy/brave app)
    [[ ! -L $root/opt/dummy/brave ]]

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
    cat > "$PREFIX/bin/droiddesk-glibc-native" <<'SH'
#!/bin/bash
printf 'native\n' >> "$TEST_LAUNCH_LOG"
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
for kind, words, tail in [('ld', [0xd2800c68, 0xd4000001], b''),
                          ('libc', [0xd2800c68, 0xd4000001], b'/etc/resolv.conf\0'),
                          ('libuv', [0xd2803520, 0x94000001, 0, 0, 0], b'')]:
    src, dst = w / (kind + '.src'), w / (kind + '.dst')
    header = bytearray(64)
    header[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', header, 18, 183)
    src.write_bytes(header + struct.pack('<%dI' % len(words), *words) + tail)
    subprocess.run(['python3', patch, kind, str(src), str(dst)], check=True)
    assert dst.read_bytes() != src.read_bytes()
blocked = {143: 'setregid', 144: 'setgid', 145: 'setreuid', 146: 'setuid',
           149: 'setresgid', 151: 'setfsuid', 152: 'setfsgid', 159: 'setgroups'}
def svc(nr):
    return struct.pack('<II', 0xd2800008 | (nr << 5), 0xd4000001)
for kind in ('ld', 'libc'):
    src, dst = w / (kind + '-setid.src'), w / (kind + '-setid.dst')
    code = svc(99) + b''.join(svc(nr) for nr in blocked) + svc(147)
    src.write_bytes(header + code + (b'/etc/resolv.conf\0' if kind == 'libc' else b''))
    result = subprocess.run(['python3', patch, kind, str(src), str(dst)],
                            check=True, capture_output=True, text=True)
    patched = dst.read_bytes()
    assert struct.unpack_from('<I', patched, 68)[0] == 0x928004a0
    for i, (nr, name) in enumerate(blocked.items(), 1):
        offset = 68 + i * 8
        expected = 0x928004a0 if kind == 'libc' else 0xd4000001
        assert struct.unpack_from('<I', patched, offset)[0] == expected, (kind, nr)
        line = f'{kind} {hex(offset)} {name}'
        assert (line in result.stdout.splitlines()) == (kind == 'libc'), (line, result.stdout)
    assert struct.unpack_from('<I', patched, 68 + 9 * 8)[0] == 0xd4000001, 'setresuid stays allowed'
    assert len(result.stdout.splitlines()) == (10 if kind == 'libc' else 1)
    # New set*id sites cannot satisfy the original required glibc-site check.
    for body in (bytes(64), b''.join(svc(nr) for nr in blocked)):
        src.write_bytes(header + body + (b'/etc/resolv.conf\0' if kind == 'libc' else b''))
        dst.unlink(missing_ok=True)
        assert subprocess.run(['python3', patch, kind, str(src), str(dst)],
                              capture_output=True).returncode != 0
        assert not dst.exists(), 'failed patch must not create output'
print('synthetic seccomp patch checks OK')
PY
printf 'GLIBC_NATIVE_OK\n'
