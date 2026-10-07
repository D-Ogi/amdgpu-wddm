#!/bin/sh
# Linux visit L1007b (2026-10-07): DP audio rate and the Azalia controller registers, the positive control for the
# Windows r19 0.33x. Read-only towards the GPU except amdgpu itself and ordinary audio playback. Never names
# amdgpu_test_ib, amdgpu_gpu_recover, amdgpu_evict_*, amdgpu_benchmark, amdgpu_preempt_ib or amdgpu_force_sclk.
#   sh run.sh gpu     apk add alsa-utils python3 if absent, modprobe amdgpu if absent, list the audio cards
#   sh run.sh rate    regs before; a 10.000 s 48 kHz tone timed with the wall clock (aplay), regs during; regs after
#   sh run.sh listen  a 1 kHz tone for 30 s at a moderate level, for the owner's headphones
set -u
B=/media/usb/l1007b
O=$B/out
mkdir -p $O
say() { echo "== $* ($(date -u +%T))"; }
dev() { aplay -l | awk '/HDMI|DisplayPort/ && /card/ {gsub(":","",$2); gsub(":","",$6); print "hw:"$2","$6; exit}'; }
case "${1:-}" in
gpu)
    say gpu
    command -v aplay >/dev/null || { apk update 2>&1 | tail -1; apk add alsa-utils python3 2>&1 | tail -2; }
    lsmod | grep -q '^amdgpu' || { modprobe amdgpu; sleep 8; }
    aplay -l 2>&1 | tee $O/aplay-l.txt
    ;;
rate)
    say rate
    python3 -c "
import math,struct,wave
for r in (48000,44100):
    w=wave.open('$O/t%d.wav'%r,'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(r)
    w.writeframes(b''.join(struct.pack('<hh',v,v) for v in (int(6000*math.sin(2*math.pi*1000*i/r)) for i in range(r*10)))); w.close()
"
    d=$(dev); echo "device $d"
    python3 $B/regs2.py before > $O/regs-before.txt 2>&1
    for r in 48000 44100; do
        ( sleep 4; python3 $B/regs2.py during$r > $O/regs-during$r.txt 2>&1 ) &
        t0=$(date +%s.%N); aplay -D "$d" $O/t$r.wav > $O/aplay$r.txt 2>&1; t1=$(date +%s.%N)
        wait
        echo "t$r: aplay $(echo "$t1 - $t0" | bc 2>/dev/null || awk "BEGIN{print $t1-$t0}") s for 10.000 s of audio" | tee -a $O/rate.txt
    done
    python3 $B/regs2.py after > $O/regs-after.txt 2>&1
    diff $O/regs-before.txt $O/regs-during48000.txt
    cat $O/regs-during48000.txt
    ;;
listen)
    say listen
    d=$(dev)
    speaker-test -D "$d" -c 2 -r 48000 -F S16_LE -t sine -f 1000 -l 5 > $O/listen.txt 2>&1
    tail -3 $O/listen.txt
    ;;
*) echo "usage: run.sh gpu|rate|listen"; exit 2 ;;
esac
