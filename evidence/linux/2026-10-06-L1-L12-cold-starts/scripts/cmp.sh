#!/bin/sh
# Count registers whose pre-driver value differs between two boots' sweeps; list the first few.
O=/media/usb/l1006c/out
s() { grep -E '^[A-Z0-9]+\.[A-Z0-9_]+ 0x' $O/$1/collect/sweep-pre.log | awk 'NF==3 {print $1, $3}' | sort; }
for p in "cold1 cold2" "cold2 cold3" "cold1 cold3" "warm-after-windows cold1" "cold3 warm-after-amdgpu"; do
	set -- $p
	s $1 > /tmp/a; s $2 > /tmp/b
	n=$(join /tmp/a /tmp/b | awk '$2 != $3' | wc -l)
	echo "== $1 vs $2: $n differ of $(wc -l < /tmp/a)"
	join /tmp/a /tmp/b | awk '$2 != $3' | head -${3:-12}
done
