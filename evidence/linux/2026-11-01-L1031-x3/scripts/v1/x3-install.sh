#!/bin/sh
# X3 install on the diagnostic stick (stick files only, no firmware boot entry). Run from /root/x3 on stick Linux.
# Builds a standalone GRUB 2.14 with chain/loadenv, keeps the Alpine loader and the old grub.cfg as backups, adds
# entry 6 (Windows chainload) after the existing six entries (indices 0-5 unchanged), the X3 counter block after the
# "set default=" line, a 1024-byte grubenv and the marker file. Leaves "set default=" as it is.
set -e
S=/media/usb; G=$S/boot/grub; B=$S/efi/boot; D=20261101
blkid /dev/sda1 | grep -q 'LABEL="BC250DIAG"' || { echo "ABORT: /dev/sda1 is not BC250DIAG"; exit 3; }
mount | grep -q " $S .*rw" || mount -o remount,rw $S
M=/usr/lib/grub/x86_64-efi
for m in chain loadenv search search_fs_file fat part_gpt part_msdos ntfs configfile normal linux echo test sleep \
         efi_gop all_video gzio; do [ -f $M/$m.mod ] || { echo "ABORT: no $m.mod"; exit 3; }; done
MODS="part_gpt part_msdos fat ntfs search search_fs_file search_label configfile normal linux chain loadenv echo test
      sleep efi_gop all_video gzio"
grub-mkstandalone -O x86_64-efi --modules="$(echo $MODS)" --locales= --themes= --fonts= \
  -o /root/x3/bootx64-x3.efi "boot/grub/grub.cfg=/root/x3/embedded.cfg"
ls -la /root/x3/bootx64-x3.efi
# new grub.cfg: line 1-2 (timeout, default) kept, X3 head, then the old entries, then entry 6
[ -f $G/grub.cfg.pre-x3-$D ] || cp $G/grub.cfg $G/grub.cfg.pre-x3-$D
OLD=$G/grub.cfg.pre-x3-$D
head -2 $OLD | grep -q '^set default=' || { echo "ABORT: line 2 of grub.cfg is not set default="; exit 3; }
{ head -2 $OLD; cat /root/x3/x3-head.cfg; tail -n +3 $OLD; cat /root/x3/x3-entry.cfg; } > /root/x3/grub.cfg.new
grub-script-check /root/x3/grub.cfg.new && echo "grub-script-check OK"
N=$(grep -c '^menuentry' /root/x3/grub.cfg.new); echo "menu entries: $N"
[ "$(grep '^menuentry' /root/x3/grub.cfg.new | sed -n 7p | cut -c1-20)" = 'menuentry "Windows (' ] || { echo "ABORT: entry 6 is not Windows"; exit 3; }
[ -f $G/grubenv ] || grub-editenv $G/grubenv create
ls -la $G/grubenv
echo "BC-250 diagnostic stick (X3 marker; embedded GRUB config searches for this file)" > $G/bc250diag.id
[ -f $B/bootx64-alpine.efi ] || cp $B/bootx64.efi $B/bootx64-alpine.efi
cp /root/x3/grub.cfg.new $G/grub.cfg
cp /root/x3/bootx64-x3.efi $B/bootx64.efi
sync
sha256sum $B/bootx64.efi $B/bootx64-alpine.efi $G/grub.cfg $OLD | sed 's|/media/usb/||'
head -24 $G/grub.cfg
