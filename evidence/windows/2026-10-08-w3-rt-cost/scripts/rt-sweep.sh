#!/usr/bin/env bash
# Operator steps of the in-game RT settings sweep (plan-474), one step per call, menu route and OCR checks of
# native-caps001/preset-sweep.sh (262-276): Esc opens the pause menu on RESUME, SETTINGS = 3 Down + E, VIDEO = 5 Down + E,
# GRAPHICS = 2 Down + E, the page opens on the Ray Tracing row, Left/Right change the highlighted row, Enter applies when
# the game asks, Esc x4 back to the world. The highlighted entry loses letters in the OCR, so checks have alternatives.
# Usage: rt-sweep.sh N graphics | keys "<actions>" | back | measure LABEL [MS=50000]
# Log: scratch/w3-rt-cost/sweep-N.log (UTC).
n="$1"; step="$2"; d=/p/bc-250/scratch/m15/native-caps001; log=/p/bc-250/scratch/w3-rt-cost/sweep-$n.log
say() { echo "$(date -u +%H:%M:%SZ) $*" | tee -a "$log"; }
g() { last=$(bash "$d/gco.sh" "$n" "$1" "${2:-25}" 2>&1); echo "$last" >> "$log"; }
need() { grep -qE -- "$1" <<<"$last" || { say "CHECK FAILED: '$1' not on screen"; echo "$last" | tail -3; exit 3; }; }
refuse() { grep -qE -- "$1" <<<"$last" && { say "CHECK FAILED: '$1' on screen"; echo "$last" | tail -3; exit 3; }; return 0; }
case "$step" in
graphics)
    g "tap:01;wait:1600;ocr"; need "SET.{0,3}NGS|TUTORIALS"; refuse "Are you sure"
    g "tapx:50;wait:200;tapx:50;wait:200;tapx:50;wait:400;tap:12;wait:1600;ocr"
    refuse "Are you sure"; need "SO.{0,2}ND|CONTROLLER SCHEME"; need "VI.{0,3}O|ACCESSIBILITY"
    g "tapx:50;wait:200;tapx:50;wait:200;tapx:50;wait:200;tapx:50;wait:200;tapx:50;wait:400;tap:12;wait:1600;ocr"
    need "INTE.{0,3}FACE|RESCALE HUD"; need "GRAP.{0,2}ICS|RAPHIC|DISPLAY"
    g "tapx:50;wait:200;tapx:50;wait:400;tap:12;wait:2200;ocr;shot:0.5"
    need "Graphics preset"; say "on the Graphics page"; echo "$last" | tail -2 ;;
keys)
    g "$3;ocr;shot:0.5" "${4:-25}"; say "keys $3"; echo "$last" | tail -2 ;;
back)
    g "tap:01;wait:1100;tap:01;wait:1100;tap:01;wait:1300;tap:01;wait:3000;ocr;shot:0.5"
    refuse "Graphics preset"; refuse "CONTROLLER SCHEME"; refuse "LOAD GAME"; say "back in the world"; echo "$last" | tail -1 ;;
measure)
    say "measure $3 call (settle 8 s first)"
    g "wait:8000;note:measure $3 start;wait:${4:-50000};note:measure $3 end;shot:0.5" 80
    grep -q "done-" <<<"$last" || { say "measure $3: no done line"; echo "$last" | tail -2; exit 3; }
    say "measured $3"; echo "$last" | tail -1 ;;
*) echo "step graphics|keys|back|measure"; exit 1 ;;
esac
