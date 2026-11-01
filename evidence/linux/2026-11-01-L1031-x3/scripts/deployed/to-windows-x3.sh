#!/bin/sh
# Run with a bounded boot waiter already active. Only the verified USB is changed.
# No asynchronous reboot: every failed prerequisite exits nonzero without reboot.
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
            echo 'X3 ERROR: failed to return stick read-only; no reboot' >&2
            result=1
        fi
    fi
    exit "$result"
}
trap finish EXIT
trap 'exit 1' HUP INT TERM
mount -o remount,rw "$S"
rw=1
python3 "$HERE/x3-files.py" prepare
grub-script-check "$G/grub.cfg.x3-new"
# Candidates and original backups were fsynced and compared by the helper.
# Counter first: a failure before cfg promotion still leaves the known Linux default.
mv "$G/grubenv.x3-new" "$G/grubenv"
mv "$G/grub.cfg.x3-new" "$G/grub.cfg"
python3 "$HERE/x3-files.py" verify
sync
mount -o remount,ro "$S"
rw=0
echo 'X3 transition verified; reboot now'
reboot
