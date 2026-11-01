#!/bin/sh
# Upgrade the already-installed X3 external config. Preserve loader, counter and default.
# Original bootstrap script is archived in recovery/x3-originals-20261101.zip.
# This revision never rebuilds or writes an EFI loader or firmware boot entry.
set -eu
S=/media/usb
G=$S/boot/grub
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 "$HERE/x3-files.py" validate
rw=0
finish() {
    result=$?
    trap - EXIT
    if [ "$rw" = 1 ]; then
        if ! mount -o remount,ro "$S"; then
            echo 'X3 ERROR: cannot return stick read-only' >&2
            result=1
        fi
    fi
    exit "$result"
}
trap finish EXIT
trap 'exit 1' HUP INT TERM
mount -o remount,rw "$S"
rw=1
python3 "$HERE/x3-files.py" upgrade
grub-script-check "$G/grub.cfg.x3-new"
mv "$G/grub.cfg.x3-new" "$G/grub.cfg"
python3 "$HERE/x3-files.py" verify-upgrade
sync
mount -o remount,ro "$S"
rw=0
echo 'X3 external config upgraded; loader/counter/default retained; no reboot'
