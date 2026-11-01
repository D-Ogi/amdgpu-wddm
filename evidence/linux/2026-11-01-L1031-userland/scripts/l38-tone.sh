#!/bin/sh
# L38 audio: DCCG/AZALIA audio DTO registers before, during and after an 8 s 1 kHz tone on the DP pin with a valid ELD.
O=/root/ul-out; mkdir -p $O; F=$O/l38-tone.txt; cd /root/ul
{
echo "# L38 tone utc=$(date -u +%FT%TZ)"
python3 regread.py l38-dto.regs before
( sleep 8 | timeout 10 speaker-test -D hw:0,3 -c 2 -r 48000 -t sine -f 1000 -l 1 -p 8000000 > $O/l38-speaker-test.txt 2>&1; echo "speaker-test exit $?" >> $O/l38-speaker-test.txt ) &
sleep 3; python3 regread.py l38-dto.regs during
cat /proc/asound/card0/pcm3p/sub0/hw_params /proc/asound/card0/pcm3p/sub0/status 2>&1
wait
python3 regread.py l38-dto.regs after
cat $O/l38-speaker-test.txt | tail -6
echo "# end utc=$(date -u +%FT%TZ)"
} > $F 2>&1
cat $F
