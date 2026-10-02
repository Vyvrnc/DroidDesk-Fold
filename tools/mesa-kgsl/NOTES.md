# Mesa with KGSL for the Debian container (fold.4 input)

Verified by hand on the Fold 7 (Debian 13 trixie in proot, Adreno 830), 2026-10-01:
Zink over Turnip/KGSL, glxinfo "zink Vulkan 1.4 (Adreno (TM) 830 (MESA_TURNIP))",
GL 4.6 / GLES 3.2, glmark2 51 (llvmpipe) -> 275. OrcaSlicer 2.4.2 runs.

## Source
https://mesa.freedesktop.org/archive/mesa-26.2.3.tar.xz (sha256 1628058a...a81f, same as the termux-packages recipe)
Patches from termux-packages `packages/mesa`: 0014 (kgsl wait_timestamp_safe), 0017 (EGL in zink without DRI3, required),
0018 (general_layout off for turnip). 0003/0006/0007/0019 are bionic-only.

## Build deps (apt --no-install-recommends, trixie arm64)
xz-utils meson ninja-build python3-mako python3-yaml python3-packaging glslang-tools bison flex libdrm-dev libx11-dev
libxext-dev libxfixes-dev libxcb1-dev libxcb-dri2-0-dev libxcb-dri3-dev libxcb-present-dev libxcb-glx0-dev
libxcb-shm0-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-randr0-dev libxcb-keysyms1-dev libx11-xcb-dev
libxshmfence-dev libxxf86vm-dev libxrandr-dev libexpat1-dev zlib1g-dev libzstd-dev pkg-config cmake libvulkan-dev
libglvnd-dev x11proto-dev gcc g++ spirv-tools libelf-dev libarchive-dev libxml2-dev
(libarchive-dev/libxml2-dev stop meson from pulling subprojects that shadow system sonames.)

## Meson
--prefix=/opt/mesa-kgsl --libdir=lib/aarch64-linux-gnu -Dbuildtype=release -Dvulkan-drivers=freedreno
-Dfreedreno-kmds=msm,kgsl -Dgallium-drivers=zink,softpipe -Dplatforms=x11 -Dglx=dri -Degl=enabled -Dgbm=enabled
-Dgles1=disabled -Dgles2=enabled -Dglvnd=enabled -Dllvm=disabled -Dxmlconfig=disabled -Dvalgrind=disabled
-Dlibunwind=disabled -Dlmsensors=disabled -Dvideo-codecs= -Dbuild-tests=false
On the phone: ninja -j3 ~35 min, 45 MB installed.

## gpu-run (per-app wrapper, overrides /etc/profile.d/droiddesk-gui.sh)
```sh
#!/bin/sh
M=/opt/mesa-kgsl
L=$M/lib/aarch64-linux-gnu
unset LIBGL_ALWAYS_SOFTWARE
export LD_LIBRARY_PATH="$L${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBGL_DRIVERS_PATH="$L/dri"
export __GLX_VENDOR_LIBRARY_NAME=mesa
export __EGL_VENDOR_LIBRARY_FILENAMES="$M/share/glvnd/egl_vendor.d/50_mesa.json"
export GBM_BACKENDS_PATH="$L/gbm"
export MESA_LOADER_DRIVER_OVERRIDE=zink
export GALLIUM_DRIVER=zink
export VK_ICD_FILENAMES="$M/share/vulkan/icd.d/freedreno_icd.aarch64.json"
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
exec "$@"
```

## Known issue
OrcaSlicer SIGSEGV on "New Project": zink_kopper_acquire_submit <- begin_rendering <- ... <- tc_batch_execute <- _tc_sync
<- tc_texture_map <- st_TexImage (threaded context flushes a draw into a kopper swapchain that is not acquired).
GALLIUM_THREAD=0 fixes it; LIBGL_KOPPER_DISABLE=true segfaults even glxgears. Candidate default for zink without DRI3,
and an upstream Mesa report (show the text to Matěj first).

## Plan
Build reproducibly (Docker, arm64 trixie under emulation) -> release asset -> debian-setup unpacks it to /opt/mesa-kgsl;
app toggle "HW acceleration" switches droiddesk-gui.sh to these variables only if /opt/mesa-kgsl exists and
/dev/kgsl-3d0 is rw; fallback llvmpipe.

## Released asset
Revision 1 (superseded): release `mesa-kgsl-26.2.3`, asset `mesa-kgsl-26.2.3-arm64.tar.xz`,
sha256 993db4dc3502632ff4e9b78a00c05238bf90b83dc95b4be7fa5ce56cd8e5b05e (6.7 MB, ~45 MB unpacked).
Built natively on the Fold 7 with the options above (subproject-extras excluded); `build.sh` is the
reproducible path for the next version. `MESA_KGSL_SHA256` in LinuxRuntime.kt must match the asset.

## Revision 2 (`mesa-kgsl-26.2.3-2`): no flicker
Revision 1 presented through kopper. Termux:X11 offers DRI3 only for AHardwareBuffer/raw-fd buffers, so
kopper fell back to software copies and GL windows flickered (FreeCAD, OrcaSlicer). Termux's own stack
does not flicker because its `mesa-zink` 22.0.5 uses zink's drisw/xlib path. Revision 2 adds `softpipe`
to `-Dgallium-drivers` (it provides the drisw/swrast winsys) and gpu-env.sh sets
`LIBGL_KOPPER_DISABLE=true`: smooth on the Fold 7, renderer still "zink … Adreno (TM) 830 (MESA_TURNIP)".
Without softpipe, kopper-off segfaults in `__glXQueryDrawable`.

Known issue: `glxgears` (clients calling `glXQueryDrawable`, e.g. swap-control queries) segfaults
(`#0 0x0` in `__glXQueryDrawable`, libGLX_mesa, drisw path). FreeCAD and OrcaSlicer are fine. Candidate
upstream report / null check.
