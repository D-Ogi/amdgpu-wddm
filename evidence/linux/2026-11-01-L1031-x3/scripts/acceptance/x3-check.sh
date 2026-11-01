#!/bin/sh
# X3 state on the stick: kernel command line, default entry, loader files, grubenv, marker.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 "$HERE/x3-files.py" validate
G=/media/usb/boot/grub
uptime; cat /proc/cmdline
grep -n '^set default=' $G/grub.cfg
ls -la /media/usb/efi/boot/
head -c 64 $G/grubenv | tr '#' '.'; echo; wc -c < $G/grubenv
ls -la $G/bc250diag.id
sha256sum /media/usb/efi/boot/bootx64.efi $G/grub.cfg | cut -c1-16
mount | grep ' /media/usb '
