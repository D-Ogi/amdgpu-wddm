#!/bin/sh
# Stick Linux -> Windows through X3: GRUB stays the first loader, default 6 (bootmgfw.efi chainload), counter clear.
# The old route (loader renamed to bootx64.off, firmware falls through to NVMe) bypasses X3; use this one instead.
S=/media/usb; G=$S/boot/grub
blkid /dev/sda1 | grep -q 'LABEL="BC250DIAG"' || { echo "ABORT: /dev/sda1 is not BC250DIAG"; exit 3; }
[ -f $G/bc250diag.id ] && [ -f $S/efi/boot/bootx64.efi ] || { echo "ABORT: X3 not installed (marker or loader missing)"; exit 3; }
[ -e $S/efi/microsoft ] && { echo "ABORT: the stick has efi/microsoft; entry 6 would chainload it"; exit 3; }
grep -q '^menuentry "Windows (NVMe bootmgfw.efi, X3 start counter)"' $G/grub.cfg || { echo "ABORT: entry 6 missing"; exit 3; }
[ "$(grep -c '^menuentry' $G/grub.cfg)" = 7 ] || { echo "ABORT: expected 7 entries"; exit 3; }
mount -o remount,rw $S
sed -i 's/^set default=.*/set default=6/' $G/grub.cfg
{ printf '# GRUB Environment Block\n'; head -c 999 /dev/zero | tr '\0' '#'; } > $G/grubenv.new
[ "$(wc -c < $G/grubenv.new)" = 1024 ] || { echo "ABORT: grubenv size"; exit 3; }
mv $G/grubenv.new $G/grubenv
sync
grep -n '^set default=' $G/grub.cfg; head -c 24 $G/grubenv; echo; ls $S/efi/boot/
mount -o remount,ro $S && echo "stick ro"
echo "reboot at $(date -u)"
(sleep 2; reboot) > /dev/null 2>&1 &
