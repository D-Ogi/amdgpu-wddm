#!/bin/sh
# Linux visit of 2026-10-07 (owner: "włączaj linuxa ... research, doinstaluj np. libdrm"). One phase per call,
# outputs under /media/usb/l1007/out. Read-only towards the GPU except what amdgpu does for ordinary clients and
# for audio playback. Never names amdgpu_test_ib, amdgpu_gpu_recover, amdgpu_evict_*, amdgpu_benchmark,
# amdgpu_preempt_ib or amdgpu_force_sclk.
#   sh run.sh setup   apk packages (alsa-utils, python3, libdrm and its tools), recorded with versions
#   sh run.sh gpu     modprobe amdgpu if absent, wait for the card, record dmesg tail and the audio cards
#   sh run.sh verbs   L41: HDA verbs to the AA01 codec through hwdep (pin sense, DIP size, ELD bytes) + ALSA ELD
#   sh run.sh tone    L38 rest: DCN audio registers before, during and after a 1 kHz tone on the DP pcm
set -u
B=/media/usb/l1007
O=$B/out
mkdir -p $O
say() { echo "== $* ($(date -u +%T))"; }
k10() { for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = k10temp ] && cat $h/temp1_input; done; }
case "${1:-}" in
setup)
    say setup
    apk update 2>&1 | tail -2
    apk add alsa-utils python3 libdrm libdrm-tests pciutils 2>&1 | tail -5
    apk info -v 2>/dev/null | grep -E '^(alsa-utils|python3|libdrm|pciutils)' | tee $O/apk-versions.txt
    ;;
gpu)
    say gpu
    lsmod | grep -q '^amdgpu' || { modprobe amdgpu; sleep 8; }
    lsmod | grep -E '^(amdgpu|snd_hda_intel|snd_hda_codec_hdmi|snd_hda_codec_atihdmi)' | tee $O/lsmod.txt
    ls /dev/dri /dev/snd | tee $O/devs.txt
    dmesg | tail -40 > $O/dmesg-gpu.txt
    aplay -l 2>&1 | tee $O/aplay-l.txt
    echo "tctl $(k10)"
    ;;
verbs)
    say verbs
    for f in /proc/asound/card*/eld#*; do echo "--- $f"; cat $f; done > $O/alsa-eld.txt 2>&1
    cat /proc/asound/card*/codec#* > $O/codec-proc.txt 2>&1
    python3 $B/verbs.py 2>&1 | tee $O/verbs.txt
    ;;
tone)
    say tone
    python3 $B/regs.py before > $O/regs-before.txt 2>&1
    dev=$(aplay -l | awk '/HDMI|DisplayPort/ && /card/ {gsub(":","",$2); gsub(":","",$6); print "hw:"$2","$6; exit}')
    echo "device $dev"
    ( speaker-test -D "$dev" -c 2 -r 48000 -F S16_LE -t sine -f 1000 -l 3 > $O/speaker-test.txt 2>&1 ) &
    sleep 2
    python3 $B/regs.py during > $O/regs-during.txt 2>&1
    wait
    python3 $B/regs.py after > $O/regs-after.txt 2>&1
    cat $O/speaker-test.txt | tail -5
    diff $O/regs-before.txt $O/regs-during.txt
    echo "tctl $(k10)"
    ;;
*) echo "usage: run.sh setup|gpu|verbs|tone"; exit 2 ;;
esac
