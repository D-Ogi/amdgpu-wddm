#!/bin/sh
# After the failed-chainload control: put back the corrected config from the stick's content-hashed backup
# (1bbc22cf..., default 4). The RAM copy of the pre-test cfg did not survive the reboot.
set -eu
S=/media/usb; G=$S/boot/grub; B=$G/grub.cfg.x3-backup-1bbc22cfd783013c
python3 /root/x3/x3-files.py validate
date -u; cat /proc/cmdline
[ "$(sha256sum "$G/grub.cfg" | cut -c1-16)" = 64f535c7159de430 ] || { echo "ABORT: cfg is not the failtest cfg"; exit 3; }
[ "$(sha256sum "$B" | cut -c1-16)" = 1bbc22cfd783013c ] || { echo "ABORT: backup hash"; exit 3; }
trap 'mount -o remount,ro /media/usb 2>/dev/null || true' EXIT
mount -o remount,rw $S
cp "$B" "$G/grub.cfg"; sync
cmp "$G/grub.cfg" "$B"
mount -o remount,ro $S
sha256sum "$G/grub.cfg" "$G/grubenv" | cut -c1-16
grep -c '^menuentry' "$G/grub.cfg"; grep -n '^set default=' "$G/grub.cfg"
echo "failtest restored"
