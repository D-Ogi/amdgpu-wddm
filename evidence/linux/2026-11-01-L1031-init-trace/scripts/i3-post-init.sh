#!/bin/sh
# i3: post-init state reads for L3 (firmware), L4 (rings/MQDs), L16/L32 (PG masks),
# L34 (scheduler), L45 (occupancy). All reads, no register writes.
OUT=/root/init-out; mkdir -p $OUT
IN=/root/init
DBG=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/amdgpu_firmware_info" ] && echo "$d"; done | head -1)
D=/sys/bus/pci/devices/0000:01:00.0
date -u > $OUT/i3-start.txt; cat /proc/uptime >> $OUT/i3-start.txt

# --- L3: firmware identity ---
cat $DBG/amdgpu_firmware_info > $OUT/l3-firmware-info.txt 2>&1
for f in /lib/firmware/amdgpu/cyan_skillfish2_*.bin; do
  [ -f "$f" ] && sha256sum "$f"
done > $OUT/l3-firmware-hashes.txt 2>&1
echo "L3: $(wc -l < $OUT/l3-firmware-info.txt) info lines, $(wc -l < $OUT/l3-firmware-hashes.txt) firmware files"

# --- L4: ring buffers and MQDs ---
for r in $DBG/amdgpu_ring_*; do
  name=$(basename "$r")
  timeout 5 cat "$r" > "$OUT/l4-${name}.txt" 2>&1
done
for f in $DBG/amdgpu_mqd_*; do
  name=$(basename "$f")
  timeout 5 od -A x -t x4 "$f" > "$OUT/l4-${name}.hex" 2>&1
done
ls $DBG/amdgpu_mqd_* >/dev/null 2>&1 || echo "no amdgpu_mqd files in debugfs" >> $OUT/l4-note.txt
rings=$(ls -1 $OUT/l4-amdgpu_ring_*.txt 2>/dev/null | wc -l)
echo "L4: $rings ring files"

# --- L16/L32: PG mask registers (broadcast) ---
if [ -f $IN/i3-pg-regs.regs ]; then
  python3 $IN/regread.py $IN/i3-pg-regs.regs broadcast > $OUT/l32-pg-broadcast.txt 2>&1
  echo "L32 broadcast: done"
else
  echo "L32: no i3-pg-regs.regs table" > $OUT/l32-pg-broadcast.txt
fi

# --- L34: scheduler and RLC/CP state ---
if [ -f $IN/i3-scheduler-regs.regs ]; then
  python3 $IN/regread.py $IN/i3-scheduler-regs.regs post-init > $OUT/l34-scheduler-state.txt 2>&1
  echo "L34: done"
else
  echo "L34: no i3-scheduler-regs.regs table" > $OUT/l34-scheduler-state.txt
fi

# --- L45: occupancy and wave limits ---
echo "== amdgpu_gpu_info" > $OUT/l45-occupancy.txt
cat $DBG/amdgpu_gpu_info >> $OUT/l45-occupancy.txt 2>&1
echo "" >> $OUT/l45-occupancy.txt
echo "== ip_discovery" >> $OUT/l45-occupancy.txt
for hw in $D/ip_discovery/die/0/*/; do
  hwid=$(basename "$hw")
  for inst in $hw*/; do
    instid=$(basename "$inst")
    echo "-- hw $hwid inst $instid"
    for f in $inst*; do
      [ -f "$f" ] && echo "  $(basename $f) = $(cat $f 2>&1)"
    done
  done
done >> $OUT/l45-occupancy.txt
echo "" >> $OUT/l45-occupancy.txt
echo "== kfd topology" >> $OUT/l45-occupancy.txt
for f in /sys/class/kfd/kfd/topology/nodes/*/properties; do
  [ -f "$f" ] && { echo "-- $f"; cat "$f"; echo ""; }
done >> $OUT/l45-occupancy.txt 2>&1
echo "" >> $OUT/l45-occupancy.txt
echo "== module parameters" >> $OUT/l45-occupancy.txt
grep . /sys/module/amdgpu/parameters/* >> $OUT/l45-occupancy.txt 2>&1
echo "L45: done"

date -u >> $OUT/i3-start.txt; cat /proc/uptime >> $OUT/i3-start.txt
echo "i3 complete"
