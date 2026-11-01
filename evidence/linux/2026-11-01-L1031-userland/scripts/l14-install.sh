#!/bin/sh
# L14: copy the linux-1031 instruments (scripts and tables, no outputs) onto the stick under bc250/tools.
DEST=/media/usb/bc250/tools
mount | grep -q ' /media/usb .*rw' || { echo "ABORT: /media/usb not mounted read-write"; exit 3; }
mkdir -p $DEST/init $DEST/display $DEST/userland $DEST/x1
cp /root/init/*.sh /root/init/*.py /root/init/*.regs /root/init/probes.txt /root/init/filters.txt /root/init/dispatch.json $DEST/init/ 2>/dev/null
cp /root/disp/*.py /root/disp/*.sh /root/disp/*.regs /root/disp/fields.txt $DEST/display/ 2>/dev/null
cp /root/ul/*.py /root/ul/*.sh /root/ul/*.regs $DEST/userland/ 2>/dev/null
cp /root/x1/*.py /root/x1/*.json $DEST/x1/ 2>/dev/null
rm -rf $DEST/*/__pycache__
cat > $DEST/README <<'EOF'
BC-250 diagnostic stick instruments (L14, linux-1031 session, 2026-11-01). Copy a directory to /root before use.
Every register offset in the tables comes from tools/regcalc on the development PC; the tables are data, not code.

init/      init trace: i1-arm.sh (kprobes from probes.txt/filters.txt, built by gen_probes.py for amdgpu.ko
           SHA256 5984c673...), i2-modprobe-trace.sh, i3-post-init.sh, regread_bank.py (per-SE/SH reads through
           the amdgpu_regs bank-select offset), i5-submit.sh + i5-submit.py (E13 dispatch), i6-paging.sh, i7-smu.sh
display/   dcnregs.py dump|pipes, dcnsample.py change-log sampler, d1..d6 scripts (D3 writes two interrupt enables
           and restores them), bar5dump.py (pre-amdgpu BAR5), gop-*.regs, dcn-all.regs, fields.txt
userland/  regread.py, bar5read.py, smnread.py, spiread.py, l7.sh pre|post, l35.sh, l49.sh host-raw|gpu,
           x2-dplink.sh, l38-tone.sh + l38-dto.regs, stick-entry.sh
x1/        x1-sdma-raw.py: SDMA COPY_LINEAR size ladder through raw CS ioctls (--route gtt2gtt|gtt2vram|vram2gtt|
           vram2vram); pin the engine with debugfs amdgpu_sdma_sched_mask (0x1 = SDMA0, 0x2 = SDMA1)
Modetest calls need a held stdin (sleep N | ...) when run through ssh sh -s.
EOF
sync
find $DEST -type f | wc -l; du -sh $DEST
