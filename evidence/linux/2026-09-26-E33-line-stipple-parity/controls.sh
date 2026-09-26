#!/bin/sh
set -eu
export LD_LIBRARY_PATH=/opt/bc250/lib VK_DRIVER_FILES=/opt/bc250/share/vulkan/icd.d/radeon_icd.x86_64.json
export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink PIGLIT_PLATFORM=surfaceless_egl MESA_SHADER_CACHE_DISABLE=true
unset RADV_DEBUG MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
set +e
timeout 45 /work/build/piglit/bin/shader_runner /work/controls/upstream-large-array.shader_test -auto -fbo > /work/parity002/positive.out 2> /work/parity002/positive.err
rc=$?; echo "$rc" > /work/parity002/positive.exit
[ "$rc" = 0 ] || exit "$rc"
grep -q '"result": "pass"' /work/parity002/positive.out || exit 90
timeout 45 /work/build/piglit/bin/line-smooth-stipple -auto -fbo > /work/parity002/line-stipple.out 2> /work/parity002/line-stipple.err
rc=$?; echo "$rc" > /work/parity002/line-stipple.exit
exit "$rc"
