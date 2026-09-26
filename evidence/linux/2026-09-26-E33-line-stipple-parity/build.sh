#!/bin/sh
set -eu
export LC_ALL=C PKG_CONFIG_PATH=/opt/bc250/lib/pkgconfig
cd /work/src/mesa-05e6c9622e135ac2aeaf56ec70222642627e2162
sha256sum src/amd/compiler/aco_lower_to_hw_instr.cpp > /work/parity002/aco-before.sha256
patch --dry-run -p1 < /work/parity002/radv-scratch-canonical-va.patch
patch -p1 < /work/parity002/radv-scratch-canonical-va.patch
sha256sum src/amd/compiler/aco_lower_to_hw_instr.cpp > /work/parity002/aco-after.sha256
ninja -j4 -C /work/build/mesa
ninja -C /work/build/mesa install
ninja -j4 -C /work/build/piglit
sha256sum /opt/bc250/lib/libvulkan_radeon.so /opt/bc250/lib/libgallium*.so /work/build/piglit/bin/line-smooth-stipple /work/build/piglit/bin/shader_runner > /work/parity002/build.sha256
dpkg-query -W > /work/parity002/packages.txt
