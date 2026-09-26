#!/bin/sh
export DISPLAY=:0 LD_LIBRARY_PATH=/opt/bc250-xorg/lib:/opt/bc250/lib:/opt/piglit/lib/piglit/lib
export GALLIUM_DRIVER=radeonsi MESA_LOADER_DRIVER_OVERRIDE=radeonsi
set +e
timeout 45 /opt/piglit/lib/piglit/bin/glx-make-current -auto -fbo > /work/parity002/make-current-native.out 2> /work/parity002/make-current-native.err
rc=$?; echo "$rc" > /work/parity002/make-current-native.exit
