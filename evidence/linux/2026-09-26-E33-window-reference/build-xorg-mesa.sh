#!/bin/sh
set -eu
export PKG_CONFIG_PATH=/opt/bc250/lib/pkgconfig
ninja -j4 -C /work/build/mesa
ninja -C /work/build/mesa install
sha256sum /opt/bc250/lib/libvulkan_radeon.so > /work/parity002/radv-restored.sha256
meson setup /work/build/mesa-xorg /work/src/mesa-05e6c9622e135ac2aeaf56ec70222642627e2162 --prefix=/opt/bc250-xorg --libdir=lib --buildtype=debugoptimized -Dllvm=disabled -Damd-use-llvm=false -Dvulkan-drivers=[] -Dgallium-drivers=radeonsi -Dplatforms=x11 -Dglx=dri -Degl=enabled -Dgbm=enabled -Dvideo-codecs=[] -Dgallium-va=disabled
ninja -j4 -C /work/build/mesa-xorg
ninja -C /work/build/mesa-xorg install
find /opt/bc250-xorg -type f -exec sha256sum {} + > /work/parity002/xorg-mesa.sha256
