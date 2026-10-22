#!/bin/bash
# Standard DroidDesk benchmark, run in Debian (debian-run / Debian terminal) on each device.
# Needs: apt install 7zip mesa-utils python3 (once). Output: ~/droiddesk-bench-<host>-<date>.txt
# Close other apps first; phone on the charger; run twice and keep the second run.
out=~/droiddesk-bench-$(hostname)-$(date +%Y%m%d-%H%M).txt
t() { local s e; s=$(date +%s.%N); "$@" >/dev/null 2>&1; e=$(date +%s.%N); awk -v a="$s" -v b="$e" 'BEGIN{printf "%.2f", b-a}'; }
# Android takes cores offline under heat and load: print the count next to every CPU number.
cores() { echo "$(nproc) cores online"; }
{
echo "== device"; uname -r; grep -m1 -i "hardware\|model name" /proc/cpuinfo; free -m | head -2
cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null
echo "gpu: $(droiddesk-gpu status 2>/dev/null | head -1)"
echo "== 7z (MIPS, Tot line)"
echo "mt1: $(7z b -mmt1 2>/dev/null | awk '/^Tot:/{print $NF}') ($(cores))"
echo "mtN: $(7z b 2>/dev/null | awk '/^Tot:/{print $NF}') ($(cores))"
echo "== proot overhead (s)"
echo "500x /bin/true: $(t bash -c 'for i in $(seq 500); do /bin/true; done') ($(cores))"
if command -v python3 >/dev/null; then
    echo "python3 import json,asyncio,ssl: $(t python3 -c 'import json,asyncio,ssl')"
else
    echo "python3 import: skipped (python3 not installed)"
fi
echo "find /usr (stat every file): $(t find /usr -xdev -type f)"
if command -v claude >/dev/null; then echo "claude --version: $(t claude --version)"; else echo "claude --version: skipped"; fi
echo "== disk (dd)"
dd if=/dev/zero of=~/.bench-dd bs=1M count=512 conv=fsync 2>&1 | tail -1 | sed 's/^/write fsync: /'
# The file is still in the page cache: O_DIRECT reads the flash; without it the number is RAM.
if dd if=~/.bench-dd of=/dev/null bs=1M iflag=direct 2>/dev/null; then
    dd if=~/.bench-dd of=/dev/null bs=1M iflag=direct 2>&1 | tail -1 | sed 's/^/read direct: /'
else
    dd if=~/.bench-dd of=/dev/null bs=1M 2>&1 | tail -1 | sed 's/^/read (page cache, not flash): /'
fi
rm -f ~/.bench-dd
echo "== GL (glxgears 10 s, vsync off)"
for mode in gpu-run llvmpipe; do
  if [ "$mode" = llvmpipe ]; then env="LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe"; pre=""; else env=""; pre="gpu-run"; fi
  echo "$mode: $(env $env vblank_mode=0 DISPLAY=:0 timeout 10 $pre glxgears 2>&1 | awk '/frames in/{f=$1}END{print f/5 " FPS (last 5 s)"}')"
  echo "  renderer: $(env $env DISPLAY=:0 $pre glxinfo -B 2>/dev/null | sed -n 's/OpenGL renderer string: //p')"
done
echo "(with hardware GL off, gpu-run is Vulkan only and its GL line is llvmpipe on purpose)"
} | tee "$out"
echo "saved $out"
