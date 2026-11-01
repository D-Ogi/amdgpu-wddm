set -e
date -u +%H:%M:%SZ
echo "=== L35 probe ==="
cd /root/ul
sh l35.sh probe
echo "=== L49 SMN ==="
sh l49.sh
echo "=== L7 firmware FB ==="
sh l7.sh pre
echo "=== done ==="
date -u +%H:%M:%SZ
