#!/bin/sh
# i4: read PG/CU mask registers per shader engine via GRBM_GFX_INDEX bank select.
# Usage: i4-per-se.sh <GI_byte_offset> <name:byte_offset> [name:byte_offset ...]
# Example: i4-per-se.sh 0x50028 SPI_PG_ENABLE_STATIC_WGP_MASK:0x170F0 CC_GC_SHADER_ARRAY_CONFIG:0x26888 ...
# The lead fills in the byte offsets from regcalc.
OUT=/root/init-out; mkdir -p $OUT
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_regs" ] && echo "$d"; done | head -1)
REGS=${DBG}amdgpu_regs
GI_OFF=$1; shift

date -u > $OUT/i4-start.txt

read_set() {
  label=$1; shift
  echo "== $label"
  for pair in "$@"; do
    name="${pair%%:*}"; off="${pair#*:}"
    v=$(python3 -c "
import os,struct
fd=os.open('$REGS',os.O_RDONLY)
d=os.pread(fd,4,int('$off',0))
os.close(fd)
print(hex(struct.unpack('<I',d)[0]))
" 2>&1)
    echo "$name $off $v"
  done
}

gi_write() {
  python3 -c "
import os,struct
fd=os.open('$REGS',os.O_WRONLY)
os.pwrite(fd,struct.pack('<I',$1),int('$GI_OFF',0))
os.close(fd)
"
}

# Broadcast (current state, expected 0xC0000000 or similar)
read_set "broadcast" "$@" >> $OUT/l32-per-se.txt

# SE0 SH0
gi_write 0x00000000
read_set "SE0_SH0" "$@" >> $OUT/l32-per-se.txt

# SE0 SH1
gi_write 0x00010000
read_set "SE0_SH1" "$@" >> $OUT/l32-per-se.txt

# SE1 SH0
gi_write 0x00000100
read_set "SE1_SH0" "$@" >> $OUT/l32-per-se.txt

# SE1 SH1
gi_write 0x00010100
read_set "SE1_SH1" "$@" >> $OUT/l32-per-se.txt

# Restore broadcast
gi_write 0xC0000000
read_set "restored_broadcast" "$@" >> $OUT/l32-per-se.txt

date -u >> $OUT/i4-start.txt
echo "i4 per-SE done"
