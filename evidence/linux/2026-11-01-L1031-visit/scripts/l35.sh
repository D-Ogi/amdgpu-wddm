#!/bin/sh
# L35: full BIOS SPI flash, read twice through the memory-mapped ROM window. No SPI command, no flashrom.
#   l35.sh probe   policy, /proc/iomem, LPC config, one-page test         (R0, < 5 s)
#   l35.sh read    two 16 MiB reads + checks (runs probe first if needed)  (R0, expected 10-60 s)
# Private output: /tmp/l35/spi-read{1,2}.bin (16 MiB each, RAM). The lead pulls /tmp/l35 to
# P:\BC-250\firmware\bios\unit-a\L35-<utc date>\ (outside the repository). Only the JSON summaries
# (/root/ul-out/l35-*.json: hashes, booleans, timings, LPC dwords) may go into evidence.
IN=/root/ul
OUT=/root/ul-out
P=/tmp/l35
mkdir -p $OUT $P
case "${1:-probe}" in
probe)
	python3 $IN/spiread.py probe $P; RC=$?
	cp $P/l35-probe.json $OUT/; echo "# probe exit $RC (0 = window readable, 4 = refused: needs iomem=relaxed)" ;;
read)
	[ -f $P/l35-probe.json ] || python3 $IN/spiread.py probe $P > /dev/null
	echo "# read start utc=$(date -u +%H:%M:%S)"
	python3 $IN/spiread.py read $P; RC=$?
	cp $P/l35-read.json $OUT/; ls -la $P
	echo "# read exit $RC end utc=$(date -u +%H:%M:%S) (0 = two identical reads with an EFS signature and unchanged LPC config)" ;;
*) echo "usage: l35.sh probe|read"; exit 2 ;;
esac
# Abort: probe exit 4 -> do not try flashrom or any other route; boot the iomem=relaxed entry (stick-entry.sh).
# A read that differs between the two passes: keep both, do not read a third time in the same boot, report.
