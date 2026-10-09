# w3-exclusive-flip-excerpt.txt

One second of The Witcher 3 (D3D12) in exclusive fullscreen at the native mode, as an xperf dumper text.
`etw-present-mode-test.py` reads it.

| | |
|---|---|
| Source capture | trial 478 of 2026-10-08, mode (a), exclusive fullscreen 1920x1200, a 20 s present-mode capture |
| Provider set | DxgKrnl at keyword mask `0xffffffffffffffff` level 5, Win32k `0x0000100000001000`, Dwm-Core `0x289`, Kernel-Process `0x10` |
| Window | 7.0 s to 8.0 s of the trace |
| Content | 1366 rows, 11 event kinds, 14 header lines |
| Processes | pid 6812 is the game, pid 1912 is the compositor |

## How to make it again

```
xperf -i gpu.etl -o big-dump.txt -a dumper
python make-fixture.py big-dump.txt --out w3-exclusive-flip-excerpt.txt --from 7.0 --to 8.0
python make-fixture.py w3-exclusive-flip-excerpt.txt --check
rm big-dump.txt
```

## What it shows

The game presented 104 times in this second. Win32k marked each of the 102 InFrame tokens with `IndependentFlip`
true and `SkipIndependentFlip` true. The kernel turned 102 of the 104 present packets into an `IndependentFlip`
with interval 0, and each of them reached `MMIOFlip` with Flags `0x82`. The flips went to three physical
addresses, 34 flips each, and the VSync DPC scanned the same three addresses. DWM consumed no present. The
capture holds no `SCHEDULE_SURFACEUPDATE` for the game.

The rules of increment 2 read the Win32k pair as a composed flip. The kernel witness of increment 3 holds on
this fixture, and the 102 presents read as `hardware flip (kernel witness; Win32k skip)`. Two presents at the
window edges have no token state in the window and read as unclassified. The test asserts all three counts.

## Why the numbers are not rewritten

`base-composed-excerpt.md` gives the reason. `make-fixture.py --check` passed on this file before it was
committed.
