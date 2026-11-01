date -u +%H:%M:%SZ
echo "=== L35 probe ==="
cd /root/ul
sh l35.sh probe 2>&1
echo "=== L35 read ==="
sh l35.sh read 2>&1
echo "=== done ==="
date -u +%H:%M:%SZ
