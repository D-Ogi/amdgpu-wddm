#!/bin/sh
# X3 live step on the stick's Linux: tools, then the corrected-config upgrade (no reboot), then readback.
set -eu
command -v grub-script-check >/dev/null 2>&1 || apk add -q --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/main --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/community grub
command -v grub-script-check
python3 --version
cat /proc/cmdline
G=/media/usb/boot/grub
echo "before:"; sha256sum $G/grub.cfg $G/grubenv /media/usb/efi/boot/bootx64.efi | cut -c1-16
grep -n '^set default=' $G/grub.cfg
sh /root/x3/x3-install.sh
echo "after:"; sha256sum $G/grub.cfg $G/grubenv | cut -c1-16
grep -n '^set default=\|^set fallback=\|^menuentry' $G/grub.cfg
sed -n '/^menuentry "Windows/,$p' $G/grub.cfg
ls $G | grep x3 || true
mount | grep ' /media/usb '
