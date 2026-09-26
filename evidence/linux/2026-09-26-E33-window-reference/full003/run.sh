#!/bin/sh
set -eu
export DISPLAY=:0 LD_LIBRARY_PATH=/opt/bc250/lib:/opt/piglit/lib/piglit/lib VK_DRIVER_FILES=/opt/bc250/share/vulkan/icd.d/radeon_icd.x86_64.json
export GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink PIGLIT_PLATFORM=glx MESA_SHADER_CACHE_DISABLE=true
export PIGLIT_BUILD_DIR=/opt/piglit/lib/piglit PIGLIT_SOURCE_DIR=/opt/piglit/lib/piglit PYTHONPATH=/opt/piglit/lib/piglit
export BC250_PIGLIT_EVENTS=/work/parity002/full003/events.jsonl BC250_STOP_FILE=/work/parity002/STOP
export PATH=/opt/piglit/lib/piglit/bin:$PATH
unset RADV_DEBUG MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
sha256sum /opt/bc250/lib/libvulkan_radeon.so /opt/bc250/lib/libgallium*.so /work/parity002/piglit*guard.py > /work/parity002/full003/identity.sha256
python3 /work/src/piglit/piglit print-cmd --format '{name}' quick > /work/parity002/full003/cases.txt
python3 /work/parity002/piglit-linux-guard.py quick /work/parity002/full003/results
