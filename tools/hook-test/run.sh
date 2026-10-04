#!/bin/bash
# Socket hook on glibc (Docker): execve of scripts whose "#!" names Termux's prefix.
#   docker run --rm -v "$PWD:/repo" debian:trixie bash /repo/tools/hook-test/run.sh
set -u
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev >/dev/null 2>&1 || exit 1
mkdir -p /tmp/stub/android && echo 'static inline int __android_log_print(int p, const char *t, const char *f, ...) { return 0; }
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_DEBUG 3' > /tmp/stub/android/log.h
gcc -shared -fPIC -O2 -Wall -I/tmp/stub -DNEW_PREFIX='"/tmp/newprefix"' -o /tmp/hook.so /repo/app/assets/socket_hook.c -ldl 2>&1 | grep -v "^$" | head -20
mkdir -p /tmp/newprefix/bin && ln -sf /bin/sh /tmp/newprefix/bin/sh && ln -sf /usr/bin/env /tmp/newprefix/bin/env
printf '#!/data/data/com.termux/files/usr/bin/sh\necho "sh-script args: $*"\n' > /root/s1 && chmod +x /root/s1
printf '#!/data/data/com.termux/files/usr/bin/env sh\necho "env-script args: $*"\n' > /root/s2 && chmod +x /root/s2
printf '#!/bin/sh\necho "normal args: $*"\n' > /root/s3 && chmod +x /root/s3
fail=0
for s in s1 s2 s3; do
  out=$(LD_PRELOAD=/tmp/hook.so env /root/$s a "b c" 2>&1); echo "$s -> $out"
  echo "$out" | grep -q 'args: a b c' || fail=1
done
out=$(LD_PRELOAD=/tmp/hook.so env /root/missing 2>&1); echo "missing -> $out"; echo "$out" | grep -q "No such file" || fail=1
out=$(env /root/s1 2>&1); echo "without hook -> $out"
[ $fail = 0 ] && echo HOOK_OK || echo HOOK_FAILED
