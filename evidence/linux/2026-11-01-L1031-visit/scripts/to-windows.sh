#!/bin/sh
# Clean unmount of the reference root, then steer the stick back to Windows (bootx64.efi -> bootx64.off) and reboot.
R=/mnt/ref/root
for d in run tmp sys proc dev; do mountpoint -q $R/$d && umount $R/$d; done
mountpoint -q /mnt/ref && umount /mnt/ref
for l in $(losetup -a | grep build-root | cut -d: -f1); do losetup -d $l; done
mountpoint -q /mnt/win && umount /mnt/win
echo "mounts left: $(grep -c -e /mnt/ref -e /mnt/win /proc/mounts)"
DEV=$(blkid | grep 'LABEL="BC250DIAG"' | cut -d: -f1 | head -1)
[ -z "$DEV" ] && { echo "stick BC250DIAG not found"; exit 1; }
TYPE=$(blkid $DEV | grep -c 'TYPE="vfat"')
TRAN=$(readlink -f /sys/class/block/$(basename $DEV) | grep -c /usb)
SIZE=$(cat /sys/class/block/$(basename $DEV)/size)
echo "dev=$DEV vfat=$TYPE usb=$TRAN part_sectors=$SIZE"
# Same bounds as to-linux.ps1: FAT32 volume of 28-34 GB on the USB bus.
[ "$TYPE" = 1 ] && [ "$TRAN" -ge 1 ] && [ "$SIZE" -ge 58720256 ] && [ "$SIZE" -le 71303168 ] || { echo "identity check failed"; exit 1; }
M=$(grep "^$DEV " /proc/mounts | awk '{print $2}' | head -1)
if [ -z "$M" ]; then mkdir -p /mnt/stick; mount $DEV /mnt/stick; M=/mnt/stick; fi
cd $M/efi/boot || exit 1
ls
[ -f bootx64.efi ] && mv bootx64.efi bootx64.off
sync; ls
cd /
echo "steered to Windows"
[ "$1" = reboot ] && { (sleep 3; reboot) >/dev/null 2>&1 & echo "rebooting"; }
