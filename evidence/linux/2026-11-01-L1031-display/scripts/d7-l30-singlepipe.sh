#!/bin/sh
# Display lane D7 (L30): single-pipe verification and register dump.
# Risk R0: read only. This runs on Boot C after `modprobe amdgpu dcdebugmask=0x1`.
set -e
cd /root/disp
echo "# d7-l30-singlepipe $(date -u '+%Y-%m-%dT%H:%M:%SZ') uptime=$(cat /proc/uptime | cut -d' ' -f1)"

# Verify the module parameter
DM=$(cat /sys/module/amdgpu/parameters/dcdebugmask 2>/dev/null || echo "UNREADABLE")
echo "dcdebugmask=$DM"
if [ "$DM" != "1" ]; then
    echo "WARNING: dcdebugmask is $DM, expected 1 (DC_DISABLE_PIPE_SPLIT)"
fi

# Pipe check: expect single pipe (HUBP0 alone, no split)
echo ""
echo "# pipes"
python3 dcnregs.py pipes > /root/disp/l30-pipes.txt 2>&1
cat /root/disp/l30-pipes.txt

# Full register dump
echo ""
echo "# dump dcn-all.regs (single-pipe reference)"
python3 dcnregs.py dump singlepipe dcn-all.regs > /root/disp/l30-dcnall.txt 2>&1

# DTN log
echo ""
echo "# DTN log"
DTN=$(ls /sys/kernel/debug/dri/*/amdgpu_dm_dtn_log 2>/dev/null | head -1)
if [ -n "$DTN" ]; then
    cat "$DTN" > /root/disp/l30-dtn.txt 2>&1
    head -30 /root/disp/l30-dtn.txt
else
    echo "no DTN log found"
fi

# Compare key registers with the split layout (Boot A values from D1)
echo ""
echo "# key single-pipe registers"
for R in HUBP0_DCHUBP_CNTL HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION HUBP3_DCHUBP_CNTL \
         MPCC0_MPCC_TOP_SEL MPCC0_MPCC_BOT_SEL MPCC0_MPCC_OPP_ID \
         MPCC3_MPCC_TOP_SEL MPCC3_MPCC_BOT_SEL MPCC3_MPCC_OPP_ID \
         ODM0_OPTC_DATA_SOURCE_SELECT ODM0_OPTC_INPUT_GLOBAL_CONTROL \
         OPPBUF0_OPPBUF_CONTROL OTG0_OTG_CONTROL; do
    python3 dcnregs.py get "$R" 2>/dev/null || echo "$R ERR"
done

echo ""
echo "# d7 done $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
