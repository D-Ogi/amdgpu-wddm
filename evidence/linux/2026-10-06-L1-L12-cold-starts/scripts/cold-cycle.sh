#!/bin/bash
# One recorded cold start of unit A (owner consent 2026-10-06 for L1/L12): clean poweroff over SSH,
# wait until the plug reads standby power, AC off for 30 s, AC on (the board jumper powers it on).
# Every step goes to power-log.jsonl next to this script.
set -u
H=$(cd "$(dirname "$0")" && pwd)
LOG=$H/power-log.jsonl
PLUG=/p/bc-250/scratch/smartplug/plug.py
W=/p/bc-250/bc250-win
label=${1:?label}
rec() { echo "{\"utc\":\"$(date -u +%FT%TZ)\",\"label\":\"$label\",\"step\":\"$1\",\"data\":$2}" | tee -a "$LOG"; }
tele() { python $PLUG telemetry 2>/dev/null | tail -1; }
rec poweroff-request "\"$(BC250_TARGET_CONFIG=/p/bc-250/secrets/linux-session/target.json python $W/tools/win/target.py run 'sync; (sleep 2; poweroff) >/dev/null 2>&1 & echo ok' </dev/null 2>&1 | tail -1)\""
for i in $(seq 1 18); do
	sleep 5
	t=$(tele); w=$(echo "$t" | python -c "import sys,json; print(json.load(sys.stdin).get('power_w', 999))" 2>/dev/null || echo 999)
	rec tele "$t"
	python -c "import sys; sys.exit(0 if float('$w') < 20 else 1)" && break
done
rec plug-off "$(python $PLUG off 2>&1 | tail -1)"
sleep 30
rec plug-on "$(python $PLUG on 2>&1 | tail -1)"
