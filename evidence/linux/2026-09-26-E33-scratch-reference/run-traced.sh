#!/bin/sh
export LD_LIBRARY_PATH=/opt/bc250/lib
export VK_DRIVER_FILES=/opt/bc250/share/vulkan/icd.d/radeon_icd.x86_64.json
export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink
export PIGLIT_PLATFORM=surfaceless_egl PIGLIT_DEFAULT_SIZE=4x4
export MESA_SHADER_CACHE_DISABLE=true RADV_DEBUG=shaders,shaderstats
set +e
export BC250_SCRATCH_TRACE=1
timeout 20 /work/build/piglit/bin/shader_runner /work/controls/bounded-array.shader_test -auto -fbo > /work/logs/traced.stdout 2> /work/logs/traced.stderr
rc=$?
echo "$rc" > /work/logs/traced.exit
