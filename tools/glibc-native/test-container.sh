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
export DD_SAVED_PATH=/termux/bin DD_SAVED_LD_LIBRARY_PATH=$libs DD_SAVED_LD_PRELOAD=$work/empty.so
printf 'int marker;\n' | gcc -shared -fPIC -x c - -o "$work/empty.so"
export PATH=/usr/bin:/bin
exec 63<"$root/etc/native-test"
"$loader" --library-path "$libs" --preload "$work/hook.so" "$root/usr/bin/probe"
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
