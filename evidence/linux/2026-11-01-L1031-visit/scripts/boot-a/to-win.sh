echo "=== steering stick back to Windows ==="
STICK=$(findmnt -nro SOURCE /media/usb 2>/dev/null || mount | awk '/\/media\/usb/{print $1}')
MP=/media/usb
mount -o remount,rw $MP 2>/dev/null || mount -o rw $STICK $MP
EFI=$MP/efi/boot/bootx64.efi
OFF=$MP/efi/boot/bootx64.off
if [ -f "$EFI" ]; then
  mv "$EFI" "$OFF" && echo "loader renamed efi -> off"
elif [ -f "$OFF" ]; then
  echo "already off"
else
  echo "ERROR: no loader file"
  exit 2
fi
ls $MP/efi/boot/
sync
echo "ready for Windows boot"
