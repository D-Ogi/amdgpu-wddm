#!/bin/sh
# Stick GRUB entry for boot B of the userland lane: a copy of the network-only entry (bc250.mode=network)
# with two kernel parameters added:
#   amd_smn_debugfs_enable  -> /sys/kernel/debug/x86/amd_smn (L49 host path under the kernel's smn_mutex)
#   iomem=relaxed           -> /dev/mem may read a ROM window that a PNP reservation marks busy (L35),
#                              needed only if `l35.sh probe` exits 4 in boot A
# Both strings are present in the stick kernel 6.18.52-0-lts (decompressed vmlinuz, 2026-11-01).
#   stick-entry.sh add       append the entry, make it the GRUB default (backup grub.cfg.pre-ul-<utc>)
#   stick-entry.sh restore   remove the entry, set the default back to the value saved by 'add'
#   stick-entry.sh show      print the menu entry titles with their index and the default
# Touches only /media/usb/boot/grub/grub.cfg (stick steering, no firmware entry). The loader file
# (bootx64.efi/.off) is not touched: a warm `reboot` lands in the new entry while the loader is .efi.
G=/media/usb/boot/grub/grub.cfg
TAG='# ul-l1101'
PARAMS='amd_smn_debugfs_enable iomem=relaxed'
show() { awk '/^menuentry /{printf "%d: %s\n", n++, $0} /^set default=/{print "  " $0}' $G; }
rw() { mount -o remount,rw /media/usb || exit 3; }
ro() { sync; mount -o remount,ro /media/usb; }
case "${1:-show}" in
show) show ;;
add)
	grep -q "$TAG begin" $G && { echo "entry already present"; show; exit 0; }
	grep -q 'bc250.mode=network' $G || { echo "ABORT: no network entry in $G"; exit 4; }
	rw
	U=$(date -u +%Y%m%dT%H%M%SZ)
	cp $G $G.pre-ul-$U
	OLD=$(sed -n 's/^set default=\([0-9]*\).*/\1/p' $G | head -1)
	N=$(grep -c '^menuentry ' $G)
	awk -v tag="$TAG" -v params="$PARAMS" -v old="$OLD" '
		{ print }
		/^menuentry / { inb = 1; buf = ""; net = 0 }
		inb { buf = buf $0 "\n"; if ($0 ~ /bc250\.mode=network/) net = 1 }
		inb && /^}/ { inb = 0; if (net && !done) { keep = buf; done = 1 } }
		END {
			n = split(keep, L, "\n")
			print tag " begin (default before: " old ")"
			for (i = 1; i < n; i++) {
				l = L[i]
				if (l ~ /^menuentry /) sub(/"[^"]*"/, "\"BC-250 network + amd_smn_debugfs_enable iomem=relaxed (userland L1101)\"", l)
				if (l ~ /^[ \t]*linux[ \t]/) l = l " " params
				print l
			}
			print tag " end"
		}' $G.pre-ul-$U > $G.new && mv $G.new $G
	sed -i "s/^set default=.*/set default=$N/" $G
	echo "$OLD" > /media/usb/boot/grub/ul-l1101.olddefault
	ro; show; echo "added as index $N; previous default $OLD; backup $G.pre-ul-$U" ;;
restore)
	grep -q "$TAG begin" $G || { echo "no entry"; show; exit 0; }
	OLD=$(cat /media/usb/boot/grub/ul-l1101.olddefault 2>/dev/null); [ -n "$OLD" ] || OLD=4
	rw
	awk -v tag="$TAG" 'index($0, tag " begin") == 1 { skip = 1 } !skip { print } index($0, tag " end") == 1 { skip = 0 }' $G > $G.new && mv $G.new $G
	sed -i "s/^set default=.*/set default=$OLD/" $G
	rm -f /media/usb/boot/grub/ul-l1101.olddefault
	ro; show; echo "removed; default back to $OLD" ;;
*) echo "usage: stick-entry.sh add|restore|show"; exit 2 ;;
esac
