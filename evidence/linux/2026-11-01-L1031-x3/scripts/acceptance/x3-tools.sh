#!/bin/sh
# Fresh RAM boot: GRUB tools for grub-script-check; drop RAM-only leftovers of an aborted failtest "set".
set -eu
command -v grub-script-check >/dev/null 2>&1 || apk add -q --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/main --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/community grub
command -v grub-script-check
rm -f /root/x3/cfg-before-failtest /root/x3/grub.cfg.failtest
sha256sum /media/usb/boot/grub/grub.cfg /media/usb/boot/grub/grubenv | cut -c1-16
