#!/bin/sh
# Run a command inside the read-only Debian build root with RADV (/opt/bc250). /tmp and /run are tmpfs;
# the stick's batch directory is bound at /mnt, so builds and outputs inside the root land on the stick.
R=/mnt/ref/root
B=${B2:-/media/usb/l1006b}
for d in dev proc sys; do mountpoint -q $R/$d || mount --bind /$d $R/$d; done
mountpoint -q $R/tmp || mount -t tmpfs tmpfs $R/tmp
mountpoint -q $R/run || mount -t tmpfs tmpfs $R/run
mountpoint -q $R/mnt || mount --bind $B $R/mnt
ICD=$(ls $R/opt/bc250/share/vulkan/icd.d/radeon_icd*.json 2>/dev/null | head -1 | sed "s#^$R##")
[ -z "$ICD" ] && ICD=$(find $R/opt/bc250 -name 'radeon_icd*.json' | head -1 | sed "s#^$R##")
exec chroot $R /usr/bin/env VK_ICD_FILENAMES=$ICD LD_LIBRARY_PATH=/opt/bc250/lib:/opt/bc250/lib/x86_64-linux-gnu \
	HOME=/tmp XDG_RUNTIME_DIR=/tmp PATH=/opt/bc250/bin:/usr/local/bin:/usr/bin:/bin "$@"
