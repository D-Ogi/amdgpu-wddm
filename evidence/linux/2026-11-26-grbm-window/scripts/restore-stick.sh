#!/bin/sh
# Linux side: puts the grubenv block that steer.ps1 saved (boot/grub/grubenv.before-grbm) back on the lab stick,
# so the next start is Windows on its persistent route again. Refuses when the saved block is missing or when the
# current block does not carry bc250_default=4 (then this script did not set it up). Usage: restore-stick.sh [check]
set -eu
DEV=$(blkid | grep 'LABEL="BC250DIAG"' | cut -d: -f1 | head -n 1)
MNT=$(awk -v d="$DEV" '$1 == d { print $2; exit }' /proc/mounts)
[ -n "$MNT" ] || { MNT=/mnt/bc250diag; mkdir -p $MNT; mount -o ro "$DEV" $MNT; }
G=$MNT/boot/grub
echo "stick $DEV at $MNT"
sha256sum $G/grubenv $G/grubenv.before-grbm
grep -a '^bc250_default=4$' $G/grubenv >/dev/null || { echo "REFUSED: grubenv has no bc250_default=4"; exit 2; }
[ "$(stat -c %s $G/grubenv.before-grbm)" = 1024 ] || { echo "REFUSED: saved block is not 1024 bytes"; exit 2; }
grep -a '^bc250_default=' $G/grubenv.before-grbm >/dev/null && { echo "REFUSED: saved block has bc250_default"; exit 2; }
[ "${1:-}" = check ] && { echo "check ok"; exit 0; }
mount -o remount,rw $MNT
dd if=$G/grubenv.before-grbm of=$G/grubenv bs=1024 count=1 conv=notrunc,fsync 2>/dev/null
sync
mount -o remount,ro $MNT || true
cmp $G/grubenv $G/grubenv.before-grbm && echo "restored: next start is the persistent Windows route"
grep -a '=' $G/grubenv
