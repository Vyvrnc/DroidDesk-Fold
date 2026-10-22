#!/bin/bash
# Standard DroidDesk benchmark, run in Debian (debian-run / Debian terminal) on each device.
# Needs: apt install 7zip mesa-utils (once). Output: ~/droiddesk-bench-<host>-<date>.txt
# Close other apps first; phone on the charger; run twice and keep the second run.
out=~/droiddesk-bench-$(hostname)-$(date +%Y%m%d-%H%M).txt
t() { local s e; s=$(date +%s.%N); "$@" >/dev/null 2>&1; e=$(date +%s.%N); awk -v a="$s" -v b="$e" 'BEGIN{printf "%.2f", b-a}'; }
{
echo "== device"; uname -r; nproc; grep -m1 -i "hardware\|model name" /proc/cpuinfo; free -m | head -2
cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null
echo "== 7z (MIPS, Tot line)"
echo "mt1: $(7z b -mmt1 2>/dev/null | awk '/^Tot:/{print $NF}')"
echo "mtN: $(7z b 2>/dev/null | awk '/^Tot:/{print $NF}')"
echo "== proot overhead (s)"
echo "500x /bin/true: $(t bash -c 'for i in $(seq 500); do /bin/true; done')"
echo "python3 import json,asyncio,ssl: $(t python3 -c 'import json,asyncio,ssl')"
echo "stat 20k files (find /usr): $(t find /usr -xdev -type f)"
echo "claude --version: $(t claude --version)"
echo "== disk (MB/s, dd)"
dd if=/dev/zero of=~/.bench-dd bs=1M count=512 conv=fsync 2>&1 | tail -1
dd if=~/.bench-dd of=/dev/null bs=1M 2>&1 | tail -1
rm -f ~/.bench-dd
echo "== GL (glxgears 10 s, vsync off)"
for mode in gpu-run llvmpipe; do
  if [ "$mode" = llvmpipe ]; then env="LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe"; pre=""; else env=""; pre="gpu-run"; fi
  echo "$mode: $(env $env vblank_mode=0 DISPLAY=:0 timeout 10 $pre glxgears 2>&1 | awk '/frames in/{f=$1}END{print f/5 " FPS (last 5 s)"}')"
  echo "  renderer: $(env $env DISPLAY=:0 $pre glxinfo -B 2>/dev/null | sed -n 's/OpenGL renderer string: //p')"
done
} | tee "$out"
echo "saved $out"
