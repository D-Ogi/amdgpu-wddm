#!/bin/sh
export DISPLAY=:0 LD_LIBRARY_PATH=/opt/bc250/lib VK_DRIVER_FILES=/opt/bc250/share/vulkan/icd.d/radeon_icd.x86_64.json
export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink PIGLIT_PLATFORM=glx MESA_SHADER_CACHE_DISABLE=true
unset RADV_DEBUG MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
set +e
timeout 45 /work/build/piglit/bin/gl-1.0-swapbuffers-behavior -auto > /work/parity002/window-radeonsi.out 2> /work/parity002/window-radeonsi.err
rc=$?; echo "$rc" > /work/parity002/window-radeonsi.exit
