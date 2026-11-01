OUT=/root/boot-a-out
date -u +%H:%M:%SZ

echo "=== X1: SDMA0 pre-released control ==="
python3 -u /root/sdma/sdma-reset-probe.py \
  --reference-dir /root/dispatch \
  --packets /root/sdma/packets.json \
  --ring 0 --release-ms 0 --wait-seconds 10 \
  > $OUT/x1-sdma0-control.txt 2>&1
echo "sdma0 exit $?"
cat $OUT/x1-sdma0-control.txt

echo "=== X1: SDMA1 pre-released control ==="
python3 -u /root/sdma/sdma-reset-probe.py \
  --reference-dir /root/dispatch \
  --packets /root/sdma/packets.json \
  --ring 1 --release-ms 0 --wait-seconds 10 \
  > $OUT/x1-sdma1-control.txt 2>&1
echo "sdma1 exit $?"
cat $OUT/x1-sdma1-control.txt

echo "=== done ==="
date -u +%H:%M:%SZ
