#!/bin/sh
# L14: copy the finished lane instruments onto the diagnostic stick as first-class commands.
#   l14-stick.sh install   copy tools to /media/usb/bc250/tools, write README
#   l14-stick.sh show      list what is on the stick
# The stick is /media/usb (FAT32). This script remounts it read-write, copies, and remounts read-only.
# Run at the END of the session, after all other steps. R0, < 10 s.
DEST=/media/usb/bc250/tools
README=/media/usb/bc250/README
rw() { mount -o remount,rw /media/usb || exit 3; }
ro() { sync; mount -o remount,ro /media/usb; }
case "${1:-show}" in
show)
	echo "# stick tools"
	if [ -d "$DEST" ]; then ls -la $DEST; else echo "(not installed)"; fi
	[ -f "$README" ] && cat $README ;;
install)
	[ -d /root/ul ] || { echo "ABORT: /root/ul not found"; exit 4; }
	rw
	mkdir -p $DEST
	# core helpers
	for f in regread.py bar5read.py smnread.py spiread.py; do
		cp /root/ul/$f $DEST/ 2>/dev/null && echo "  $f"
	done
	# register tables
	for f in l7-fwfb.regs l49-smn.regs x2-dplink.regs; do
		cp /root/ul/$f $DEST/ 2>/dev/null && echo "  $f"
	done
	# lane scripts
	for f in l7.sh l35.sh l35-compare.py l49.sh x2-dplink.sh stick-entry.sh; do
		cp /root/ul/$f $DEST/ 2>/dev/null && echo "  $f"
	done
	cat > $README <<'READMEEOF'
BC-250 diagnostic stick tools (L14, installed by l14-stick.sh)

Usage: these are the instruments from the linux-1031 session. The lead
pushes them to /root/ul and runs them through linux-xfer.py; copies here
are for persistence across boots. Run from /root/ul, not from this stick.

  regread.py <table> [label]     read named GPU registers through debugfs
  bar5read.py <table> [label]    read named GPU registers through BAR5 (pre-driver)
  smnread.py <table> <mode>      read SMN registers (gpu|dbgfs|host-raw)
  spiread.py probe|read <dir>    SPI flash through the ROM window (no SPI command)
  l7.sh pre|post                 memory layout before/after amdgpu
  l35.sh probe|read              BIOS SPI flash read (two passes)
  l49.sh host-raw|gpu|dbgfs      APU-to-FCH link status
  x2-dplink.sh                   DP link training policy
  stick-entry.sh add|restore     manage a custom GRUB entry
READMEEOF
	ro
	echo "installed to $DEST" ;;
*) echo "usage: l14-stick.sh install|show"; exit 2 ;;
esac
