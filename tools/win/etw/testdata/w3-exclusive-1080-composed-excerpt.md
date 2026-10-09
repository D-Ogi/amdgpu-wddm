# w3-exclusive-1080-composed-excerpt.txt

One second of The Witcher 3 (D3D12) in exclusive fullscreen at 1920x1080, where DWM composed the game, as an
xperf dumper text. `etw-present-mode-test.py` reads it.

| | |
|---|---|
| Source capture | trial 478 of 2026-10-08, mode (b), exclusive fullscreen 1920x1080, a 20 s present-mode capture |
| Provider set | the provider set of `w3-exclusive-flip-excerpt.md` |
| Window | 7.0 s to 8.0 s of the trace |
| Content | 1585 rows, 14 event kinds, 16 header lines |
| Processes | pid 6812 is the game, pid 1912 is the compositor |

## How to make it again

```
xperf -i gpu.etl -o big-dump.txt -a dumper
python make-fixture.py big-dump.txt --out w3-exclusive-1080-composed-excerpt.txt --from 7.0 --to 8.0
python make-fixture.py w3-exclusive-1080-composed-excerpt.txt --check
rm big-dump.txt
```

## What it shows

The game presented 95 times in this second. Win32k marked every InFrame token with `IndependentFlip` false. The
trace holds no `IndependentFlip` event for the game, and no `MMIOFlip` of a game packet. DWM consumed 58 presents
through `Windowed_Dx_Flip_Consumed` and reported direct flip false for each of them. The other 37 presents read as
unclassified: 34 have a token state and no consumption, and 3 have no token state in the window.

The D3D12 shell made a composed primary in this mode. The scan-out experiment of the trial named 1920x1200 and
the chain was 1920x1080. The kernel witness must read `NOT-HELD` on this fixture, and no label may move. It is the
negative control of `w3-exclusive-flip-excerpt.txt`, from the same game, the same session and the same capture
recipe.

## Why the numbers are not rewritten

`base-composed-excerpt.md` gives the reason. `make-fixture.py --check` passed on this file before it was
committed.
