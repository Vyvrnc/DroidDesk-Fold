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
export DD_ROOTFS=$root DD_TMP=$work/tmp DD_PLIB=$work/plib DD_LIBPATH=$libs
export DD_SAVED_PREFIX=/termux/usr DD_SAVED_TERMUX_VERSION=0.118 DD_SAVED_TERMUX__TEST="value with spaces"
export DD_SAVED_PATH=/termux/bin DD_SAVED_LD_LIBRARY_PATH=$libs DD_SAVED_LD_PRELOAD=$work/empty.so
printf 'int marker;\n' | gcc -shared -fPIC -x c - -o "$work/empty.so"
export PATH=/usr/bin:/bin
exec 63<"$root/etc/native-test"
"$loader" --library-path "$libs" --preload "$work/hook.so" "$root/usr/bin/probe"
# Source only launcher functions; exercise the same code as the shipped commands.
(
    export PREFIX=$work/termux
    source <(sed '/^command=/,$d' /repo/app/android/app/src/main/assets/droiddesk/droiddesk-glibc-native)
    R=$root N=$PREFIX/opt/glibc-native T=$work/tmp
    PLIB=$N/plib
    LIBPATH=$PLIB:$N/lib:$libs
    mkdir -p "$N"/{lib,bin,locale,plib} "$PREFIX/bin" "$root/usr/lib/aarch64-linux-gnu"
    ln -s "$loader" "$PLIB/ld.so"
    ln -s "$work/hook.so" "$N/lib/pathhook.so"
    ln -s /usr/bin/env "$PREFIX/bin/env"
    cp /usr/bin/env "$root/usr/bin/env"
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
    export TERMUX_VERSION=0.118 TERMUX__TEST='value with spaces'
    (run_native /usr/bin/android-marker android)
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
src.write_bytes(header + bytes(64))
assert subprocess.run(['python3', patch, 'ld', str(src), str(dst)]).returncode != 0
PY
printf 'GLIBC_NATIVE_OK\n'
