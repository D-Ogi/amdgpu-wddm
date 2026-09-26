#!/bin/sh
export DISPLAY=:0 LD_LIBRARY_PATH=/opt/bc250/lib
export VK_DRIVER_FILES=/opt/bc250/share/vulkan/icd.d/radeon_icd.x86_64.json
export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink
export MESA_SHADER_CACHE_DISABLE=true
unset RADV_DEBUG MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
set +e
for platform in glx x11_egl surfaceless_egl; do
 timeout 15 wflinfo --platform=$platform --api=gl > /work/parity002/wflinfo-$platform.out 2> /work/parity002/wflinfo-$platform.err
 echo "$?" > /work/parity002/wflinfo-$platform.exit
done
