#!/bin/sh
# Read-only: mount the Windows NTFS volume and the Debian/Mesa build root inside it, list what can load the GPU.
set -e
modprobe ntfs3 2>/dev/null || true
modprobe loop 2>/dev/null || true
mkdir -p /mnt/win /mnt/ref
WIN=$(blkid | grep -i ntfs | sort -t: -k1 | awk -F: '{print $1}' | while read d; do s=$(blockdev --getsize64 $d 2>/dev/null || echo 0); echo "$s $d"; done | sort -n | tail -1 | awk '{print $2}')
echo "ntfs volume: $WIN"
mountpoint -q /mnt/win || mount -t ntfs3 -o ro $WIN /mnt/win
IMG=/mnt/win/BC250/m12/linux-reference001/build-root.ext4
ls -la $IMG
mountpoint -q /mnt/ref || mount -o ro,loop,noload $IMG /mnt/ref
ls /mnt/ref
ls /mnt/ref/work 2>/dev/null | head
for b in vkcube vkmark vulkaninfo glmark2 deqp-vk piglit; do p=$(chroot /mnt/ref sh -c "command -v $b" 2>/dev/null || true); echo "$b: $p"; done
find /mnt/ref/work /mnt/ref/opt -maxdepth 4 -type f \( -name 'deqp-vk' -o -name 'vkcube*' -o -name '*membw*' -o -name 'glmark2*' \) 2>/dev/null | head
ls /mnt/ref/usr/share/vulkan/icd.d /mnt/ref/opt/*/share/vulkan/icd.d 2>/dev/null
