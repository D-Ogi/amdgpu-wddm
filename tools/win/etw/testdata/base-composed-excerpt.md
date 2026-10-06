# base-composed-excerpt.txt

One second of the composed baseline, as an xperf dumper text. `etw-present-mode-test.py` reads it.

| | |
|---|---|
| Source capture | `scratch/train/etw-iflip-111536/gpu.etl`, the b18r1 scan-out arm of 2026-10-05 |
| Provider set | `scratch/train/b18r1-iflip-etw.ps1` lines 10 to 13 |
| Window | 2.0 s to 3.0 s of the trace |
| Content | 1200 rows, 14 event kinds, 18 header lines |
| Processes | pid 4476 is the D3D12 client, pid 1956 is the compositor |

## How to make it again

```
xperf -i gpu.etl -o big-dump.txt -a dumper
python make-fixture.py big-dump.txt --out base-composed-excerpt.txt --from 2.0 --to 3.0
python make-fixture.py base-composed-excerpt.txt --check
rm big-dump.txt
```

`make-fixture.py` keeps only the event kinds `etw-present-mode.py` reads. It also drops every `QueuePacket`
start that is not a present, because the parser ignores those rows and they are nine tenths of the text.

## What it shows

The client presented 60 times in this second. Every present call carries Flags `0x9000`, which is
`RedirectedFlip` with `PresentCountValid`. Every `PresentHistoryDetailed` record carries Model 2, which is
`REDIRECTED_FLIP`. DWM answered `bDirectFlip 1` with `bIndependentFlip 0`, and then consumed 58 of the 60 frames
through `Windowed_Dx_Flip_Consumed`. The trace holds no `DxgKrnl/IndependentFlip` event. The plane read three
addresses, and all three belong to the compositor.

Two presents at the window edges read as unclassified. Their token states and their consumption fall outside the
one-second window. That is a property of the excerpt, not of the parser, and the test asserts both counts.

## Why the numbers are not rewritten

Timestamps, process ids, thread ids, kernel pointers and physical addresses stay as the capture wrote them. The
fixture is evidence of what these shapes look like on this driver. A rewritten number would make the test prove
nothing.

`make-fixture.py --check` refuses a fixture that holds a Windows SID, an IP address, a UNC path or a user
profile path. None of these event kinds carries one. The check runs over the file anyway, before anybody commits
it.
