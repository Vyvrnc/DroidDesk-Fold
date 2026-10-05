#!/bin/bash
# Applies the fix to the 1.3.0 tree in the image and writes the diff to /work.
set -e
cd /src/virglrenderer
python3 - <<'PY'
p = 'src/vrend/vrend_renderer.c'
s = open(p).read()
old = '''static bool use_integer(void) {
   if (getenv("VIRGL_USE_INTEGER"))
      return true;
'''
new = '''static bool use_integer(void) {
   if (getenv("VIRGL_USE_INTEGER"))
      return true;

   /* GLES (unlike desktop GL drivers in practice) gives undefined results when a
    * vertex attribute specified with glVertexAttribIPointer/IFormat is read through a
    * float shader input (type mismatch is undefined in GLES), same for integer color
    * buffers written from a float output. Mali showed this first (the "ARM" check
    * below); ANGLE on Vulkan does it too: an `in int` attribute arrives as 0, so e.g.
    * OrcaSlicer/libvgcode toolpaths collapse to nothing. Declaring the inputs with
    * their real integer type is always correct, so do it for every GLES host. */
   if (vrend_state.use_gles)
      return true;
'''
assert old in s
s = s.replace(old, new, 1)
open(p, 'w').write(s)
PY
git diff > /work/0001-vrend-declare-integer-vertex-attribs-on-GLES-hosts.patch
ninja -C build >/dev/null
echo patched+rebuilt
