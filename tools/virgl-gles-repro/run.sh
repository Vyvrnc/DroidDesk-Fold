#!/bin/bash
# Runs inside the container. $1 = path to virgl build dir (default /src/virglrenderer/build)
B=${1:-/src/virglrenderer/build}
cd /work
gcc -O0 -g -o /tmp/tbo_test tbo_test.c -lEGL -lGL || exit 2
V="full rgba uint r32f noninst fs intattr"
echo "== llvmpipe (direct) =="
for v in $V; do LIBGL_ALWAYS_SOFTWARE=1 /tmp/tbo_test $v 2>/dev/null; done
rm -f /tmp/.virgl_test
env LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless ${SERVER_ENV} VREND_DEBUG=${VREND_DEBUG:-err} \
  $B/vtest/virgl_test_server --use-egl-surfaceless --use-gles --multi-clients > /tmp/server.log 2>&1 &
SP=$!; sleep 1
echo "== virpipe -> virgl_test_server --use-gles (Mesa GLES/llvmpipe host) =="
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=virpipe MESA_GL_VERSION_OVERRIDE=4.3 MESA_GLSL_VERSION_OVERRIDE=430
for v in $V; do /tmp/tbo_test $v 2>>/tmp/client.log; done
kill $SP; wait $SP 2>/dev/null
cp /tmp/server.log /work/out/server-${TAG:-run}.log; cp /tmp/client.log /work/out/client-${TAG:-run}.log 2>/dev/null
