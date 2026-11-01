OUT=/root/boot-a-out
vulkaninfo --summary > $OUT/l33-vulkaninfo-summary.txt 2>&1 || true
head -30 $OUT/l33-vulkaninfo-summary.txt
echo "---"
vulkaninfo > $OUT/l33-vulkaninfo-full.txt 2>&1 || true
wc -l $OUT/l33-vulkaninfo-full.txt
