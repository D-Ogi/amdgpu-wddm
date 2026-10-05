#!/usr/bin/env bash
# BD-051 self-heal: if an authenticated ssh command does not answer, collect the stall forensics over the
# emergency channel (listener stack, children tables, exit codes, pipe holders, 4688/4689), then restart sshd and
# check again. Usage: sshd-heal.sh EVIDENCE_DIR   (exit 0 = ssh answers, 1 = healed, 2 = still down)
here=$(cd "$(dirname "$0")" && pwd) || exit 2
emerg="$here/../lab-emerg.py"
target="$here/../../target.py"
dir="$1"; mkdir -p "$dir"
ok() { timeout 30 python "$target" run hostname 2>/dev/null | grep -q .; }
ok && exit 0
ts=$(date -u +%H%M%S)
echo "$(date -u +%H:%M:%S) ssh does not answer: forensics and restart-sshd" | tee -a "$dir/heal.log"
timeout 150 python "$emerg" ps "$here/sshd-stall-forensics.ps1" 120 > "$dir/forensics-$ts.json" 2>&1
python -c "
import json,sys
d=json.load(open(sys.argv[1])); r=d.get('result',d)
print('\n'.join(r['output']) if isinstance(r,dict) and 'output' in r else d)" "$dir/forensics-$ts.json" > "$dir/forensics-$ts.txt" 2>&1
timeout 90 python "$emerg" restart-sshd >> "$dir/heal.log" 2>&1
sleep 5
python "$target" forget > /dev/null 2>&1
if ok; then echo "$(date -u +%H:%M:%S) healed" | tee -a "$dir/heal.log"; exit 1; fi
echo "$(date -u +%H:%M:%S) still down after restart-sshd" | tee -a "$dir/heal.log"; exit 2
