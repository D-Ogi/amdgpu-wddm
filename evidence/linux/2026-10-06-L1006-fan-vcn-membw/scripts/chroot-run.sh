#!/bin/sh
# Run a command inside the read-only Debian build root with RADV (/opt/bc250), writable /tmp and /work-out on tmpfs.
R=/mnt/ref/root
for d in dev proc sys; do mountpoint -q $R/$d || mount --bind /$d $R/$d; done
mountpoint -q $R/tmp || mount -t tmpfs tmpfs $R/tmp
mountpoint -q $R/run || mount -t tmpfs tmpfs $R/run
export VK_ICD_FILENAMES=$(ls $R/opt/bc250/share/vulkan/icd.d/radeon_icd*.json 2>/dev/null | head -1 | sed "s#^$R##")
[ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$(find $R/opt/bc250 -name 'radeon_icd*.json' | head -1 | sed "s#^$R##")
export LD_LIBRARY_PATH=/opt/bc250/lib:/opt/bc250/lib/x86_64-linux-gnu
export HOME=/tmp XDG_RUNTIME_DIR=/tmp
echo "ICD $VK_ICD_FILENAMES" >&2
exec chroot $R /usr/bin/env VK_ICD_FILENAMES=$VK_ICD_FILENAMES LD_LIBRARY_PATH=$LD_LIBRARY_PATH HOME=/tmp XDG_RUNTIME_DIR=/tmp "$@"
