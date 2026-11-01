echo "=== Vulkan ICD check ==="
ls /usr/share/vulkan/icd.d/ 2>&1 || echo "no ICDs"
which vulkaninfo 2>/dev/null || echo "no vulkaninfo"
echo "=== NVMe ==="
lsblk -o NAME,SIZE,FSTYPE,LABEL | grep -E 'nvme|NAME'
echo "=== reference root check ==="
mount | grep ntfs || echo "ntfs not mounted"
ls /dev/nvme0n1p3 2>/dev/null && echo "p3 exists" || echo "no p3"
