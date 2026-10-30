#!/usr/bin/env bash
# Builds exec-probe.apk (targetSdk 36): which ways of starting native programs survive W^X.
# Run from the repo root. Output: tools/exec-probe/out/exec-probe.apk
set -euo pipefail
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }
REPO=$(to_host "$PWD")
NDK=28.2.13676358

# 1) glibc side in Debian trixie arm64: hello-glibc, patched ld.so (libld.so) and libc.so.6.
MSYS_NO_PATHCONV=1 docker run --rm --platform linux/arm64 -v "$REPO:/repo" debian:trixie bash -c '
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev python3 >/dev/null 2>&1
o=/repo/tools/exec-probe/out; rm -rf $o; mkdir -p $o/lib/arm64-v8a $o/assets/glibc
gcc -O2 -Wall -o $o/assets/hello-glibc /repo/tools/exec-probe/hello.c
p=/repo/app/android/app/src/main/assets/droiddesk/glibc-native/patch-glibc.py
python3 $p ld /usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 $o/lib/arm64-v8a/libld.so
python3 $p libc /usr/lib/aarch64-linux-gnu/libc.so.6 $o/assets/glibc/libc.so.6
dpkg-query -W libc6
'

# 2) Android side: bionic programs, Java, APK.
MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO:/repo" -v droiddesk-ndk:/opt/android-sdk-linux/ndk ghcr.io/cirruslabs/flutter:stable bash -c "
set -e
yes | sdkmanager --licenses >/dev/null 2>&1 || true
[ -d /opt/android-sdk-linux/ndk/$NDK ] || sdkmanager 'ndk;$NDK' >/dev/null
[ -d /opt/android-sdk-linux/platforms/android-36 ] || sdkmanager 'platforms;android-36' >/dev/null
BT=\$(ls -d /opt/android-sdk-linux/build-tools/* | sort -V | tail -1)
JAR=/opt/android-sdk-linux/platforms/android-36/android.jar
T=/opt/android-sdk-linux/ndk/$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
p=/repo/tools/exec-probe; o=\$p/out; w=/tmp/w; mkdir -p \$w/classes
\$T/aarch64-linux-android29-clang -O2 -pie -o \$o/assets/hello-bionic \$p/hello.c
\$T/aarch64-linux-android29-clang -O2 -pie -o \$o/lib/arm64-v8a/libhello.so \$p/hello.c
javac -source 11 -target 11 -classpath \$JAR -d \$w/classes \$(find \$p/src -name '*.java') 2>&1 | grep -v warning || true
\$BT/d8 --min-api 29 --lib \$JAR --output \$w \$(find \$w/classes -name '*.class')
\$BT/aapt2 link --manifest \$p/AndroidManifest.xml -I \$JAR -A \$o/assets -o \$w/base.apk
cd \$w && cp base.apk unsigned.apk && zip -q unsigned.apk classes.dex
cd \$o && zip -q -r \$w/unsigned.apk lib
\$BT/zipalign -f -p 4 \$w/unsigned.apk \$w/aligned.apk
keytool -genkeypair -keystore \$w/k.jks -storepass probepw -keypass probepw -alias probe -keyalg RSA -dname CN=probe -validity 30 >/dev/null 2>&1
\$BT/apksigner sign --ks \$w/k.jks --ks-pass pass:probepw --out \$o/exec-probe.apk \$w/aligned.apk
ls -l \$o/exec-probe.apk
"
