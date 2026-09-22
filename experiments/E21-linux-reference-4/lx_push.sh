#!/bin/bash
# Push the session's scripts into /tmp/lx on the probe (tar through ssh; scp exists now too).
cd /p/BC-250 || exit 1
eval "$(sed -n '4p' scratch/tmp/risky_run.sh | sed -E "${BC250_PROBE:+s/root@[0-9.]+/root@${BC250_PROBE}/}; s/-o BatchMode=yes/-o BatchMode=yes -o StrictHostKeyChecking=accept-new/")"
STAGE=/p/BC-250/scratch/lx/stage
rm -rf "$STAGE"; mkdir -p "$STAGE"
R=bc250-win
cp scratch/tmp/linux_session_collect.sh "$STAGE/"
cp $R/experiments/E13-linux-reference-2/{regs2.py,lists.json,info.py,info.json,dispatch.py,dispatch.json,arm_kprobes.sh} "$STAGE/"
cp $R/experiments/E17-linux-reference-3/{vmlists.json,e17_vm.py,pt_walk.py,session.sh} "$STAGE/"
cp $R/evidence/linux/2026-09-21-E03-init-trace/sweep.json $R/evidence/linux/2026-09-21-E03-init-trace/skip-conservative.txt "$STAGE/" 2>/dev/null
D=$R/experiments/E20-first-picture-full-wddm/linux
[ -d "$D" ] && cp "$D"/*.py "$D"/*.json "$STAGE/" 2>/dev/null
cp $R/tools/diagusb/sweep_stream.py "$STAGE/" 2>/dev/null
cp /p/BC-250/scratch/lx/extra/* "$STAGE/" 2>/dev/null
ls "$STAGE" | tr '\n' ' '; echo
tar -C "$STAGE" -cf - . | $BC250_SSH "mkdir -p /tmp/lx && tar -C /tmp/lx -xf - && ls /tmp/lx | wc -l"
