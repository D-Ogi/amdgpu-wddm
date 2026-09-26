#!/bin/sh
export LD_LIBRARY_PATH=/opt/bc250/lib
export VK_DRIVER_FILES=/opt/bc250/share/vulkan/icd.d/radeon_icd.x86_64.json
export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink
export PIGLIT_PLATFORM=surfaceless_egl
export MESA_SHADER_CACHE_DISABLE=true RADV_DEBUG=shaders,shaderstats
set +e
timeout 20 /work/build/piglit/bin/shader_runner /work/controls/upstream-large-array.shader_test -auto -fbo > /work/logs/upstream.stdout 2> /work/logs/upstream.stderr
rc=$?
echo "$rc" > /work/logs/upstream.exit
