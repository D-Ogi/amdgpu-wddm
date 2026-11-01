#!/bin/sh
# i0: pull the amdgpu module and kernel BTF from unit A (before modprobe).
# Run: linux-xfer.py run init/i0-pull-module.sh
OUT=/root/init-i0; mkdir -p $OUT
cp /lib/modules/$(uname -r)/kernel/drivers/gpu/drm/amd/amdgpu/amdgpu.ko* $OUT/
[ -f /sys/kernel/btf/vmlinux ] && cp /sys/kernel/btf/vmlinux $OUT/vmlinux.btf
uname -r > $OUT/uname.txt
sha256sum $OUT/amdgpu.ko* > $OUT/sha256.txt
ls -la $OUT/
echo "i0 done"
