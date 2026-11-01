#!/bin/sh
# X3 fault control: a temporary entry 7 runs entry 6's logic but chainloads a missing file. Expected: GRUB
# falls back to entry 4 (network-only Linux on the USB). "restore" puts back the exact pre-test grub.cfg.
set -eu
S=/media/usb; G=$S/boot/grub; KEEP=/root/x3/cfg-before-failtest
python3 /root/x3/x3-files.py validate
trap 'mount -o remount,ro /media/usb 2>/dev/null || true' EXIT
case "${1:-}" in
set)
  [ -e "$KEEP" ] && { echo "ABORT: $KEEP exists (test already set?)"; exit 3; }
  cp "$G/grub.cfg" "$KEEP"
  [ "$(grep -c '^set default=[0-6]$' "$KEEP")" = 1 ] || { echo "ABORT: expected one default line before the test"; exit 3; }
  { sed 's/^set default=[0-6]$/set default=7/' "$KEEP"; cat <<'EOF'

menuentry "X3 TEST: failed Windows chainload (temporary)" {
  unset win_esp
  if search --no-floppy --file --set=win_esp /EFI/Microsoft/Boot/bootmgfw.efi; then
    set root=${win_esp}
    chainloader /EFI/Microsoft/Boot/x3-missing-test.efi
    set root=${stick}
  else
    false
  fi
}
EOF
  } > /root/x3/grub.cfg.failtest
  grub-script-check /root/x3/grub.cfg.failtest
  [ "$(grep -c '^menuentry' /root/x3/grub.cfg.failtest)" = 8 ] || { echo "ABORT: entry count"; exit 3; }
  mount -o remount,rw $S
  cp /root/x3/grub.cfg.failtest "$G/grub.cfg"; sync
  cmp "$G/grub.cfg" /root/x3/grub.cfg.failtest
  mount -o remount,ro $S
  grep -n '^set default=' "$G/grub.cfg"; sha256sum "$KEEP" "$G/grub.cfg" | cut -c1-16
  echo "failtest set; reboot now"
  ;;
restore)
  [ -f "$KEEP" ] || { echo "ABORT: no $KEEP"; exit 3; }
  mount -o remount,rw $S
  cp "$KEEP" "$G/grub.cfg"; sync
  cmp "$G/grub.cfg" "$KEEP"
  mount -o remount,ro $S
  sha256sum "$G/grub.cfg" | cut -c1-16; grep -n '^set default=' "$G/grub.cfg"
  mv "$KEEP" "$KEEP.done"
  echo "failtest restored"
  ;;
*) echo "usage: x3-failtest.sh set|restore"; exit 2 ;;
esac
