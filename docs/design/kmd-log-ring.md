# The KMD log ring: how long it holds, and the rules for a hot line

The lab machine has no kernel debugger and no DebugView. The KMD therefore keeps every log line in
a ring inside its own image, and `bc250kmd_cli log` reads the ring back. The ring is the only trail
of a device start, a mode set or a hang on that machine.

## What the ring holds

The ring has 1024 lines of 160 bytes. The first 256 lines are the head. The driver never overwrites
the head while it stays loaded, because the order of the start-up is the evidence. The other 768
lines are the tail, and each new line overwrites the oldest line of the tail. The driver counts the
lines that the tail overwrites, so a gap is always visible.

The ring holds a number of lines, not a number of seconds. A driver that writes two lines a second
keeps about six minutes in the tail. A driver that writes one line a second keeps about thirteen
minutes. The lab measured the first number on 2026-10-08 (defect BD-097): 708 of the 768 tail lines
were one line of the SDMA paging submit path, the tail held 354 s, and the two mode-set lines of an
hour before were gone.

## The rules for a line on a hot path

A hot path is a path that runs for each frame, each submission or each paging operation. One
unconditional log line there spends the whole ring on itself. The driver has three rules for such a
line, and a new hot line must use one of them.

| Rule | Where | What it does |
|---|---|---|
| A gate | `HotSubmitLog` in `gfx.c` | The line is off. An experiment that wants it opens the gate for its run |
| A count cap | `BC250_WDDM_LOG_CALLS`, `BC250_DCN_LOG_CALLS`, `BC250_VIDMM_LOG_CALLS` | The first eight calls of the DDI write their line. The calls after them write nothing |
| A rate limit | `log_rate.h` | The first eight calls of the device start write their line. After them one line a minute carries the count |

The rate limit is for a line that must keep a cadence, because the reader of the trail must see that
the path still works. One minute is the cadence that the telemetry block of the same defect got. A
call that follows 2 s of quiet also writes its line, so the first work after an idle desktop is in
the log, but only while the minute still has room for a second line. That cap is what bounds the
rule: two lines a minute is 120 lines an hour, whatever shape the traffic has. The measured idle
traffic gives 67 lines an hour, against 7200 before. The host test is
`driver/kmd/test/log_rate_test.c`, and `tools/quality/quick.ps1` runs it as the `log-rate` gate with
a negative control.

Every rule keeps the error paths. A refusal, a timeout and a fault write their line in full, every
time.

The log summary states what each rule left out. `wddm profile: hot submit log lines left out` is the
gate's count. `wddm profile: paging submit lines` is the rate limit's count of submits, silent
submits and summary lines. A quiet log must never read as a quiet node.

## The summary, beside the ring

One log summary is about 320 lines. In the ring that is 42 % of the tail for each call. A caller
that asks for a summary every few seconds rotates the whole ring in about 12 s. The overlay polled
the summary that way until KMD 0.7.213, and the b23 lab read found the cost in the ring.

From KMD 0.7.216.24 the polling form of the escape writes the block beside the ring:

- `bc250kmd_cli log summary only` sends `From` as `BC250_LOG_FROM_SUMMARY`. The driver writes the
  block into storage of its own, answers `SummaryFrom` as the sequence of the block's first line,
  and adds one rate-limited line to the ring. That line says how many lines the block holds, at
  which ring sequence the caller took it, and the sequence to read it from.
- `bc250kmd_cli log summary` sends a position of its own. The driver writes the block into the ring,
  where a reader of the whole trail wants it, between the lines around it.

`BC250_ESCAPE_GET_LOG` reads the block exactly as it reads the ring. The caller asks for the
sequence that the driver answered with, pages on with `Next`, and stops when `Returned` is 0. The
sequence numbers of the block are above every sequence that the ring can give, so no line of the
block reads as a ring line.

Two rules keep the block from taking evidence out of the ring:

- A line of a DPC always goes into the ring. The driver diverts the lines of the thread that asked
  for the summary, but a DPC runs in the context of the thread that its processor was running. The
  watchdog DPCs write the `FENCE TIMEOUT` lines, which are the evidence of a hang, so the driver
  sends a line to the ring if `KeIsExecutingDpc` is true, whoever the current thread is.
- The driver holds one block at a time, and each summary answers from the next block of the summary
  space (`BC250_LOG_SUMMARY_BLOCKS` of them before the numbers start again). A page read is not
  serialized against the summary escape, because the pages go without adapter synchronization, so
  one caller can take a summary while another caller pages the block before it. That reader gets an
  empty page, and the escape answers `SummaryFrom` as the block that the driver holds now.
  `bc250kmd_cli` prints one line to say that the block was replaced. A page of one summary together
  with a page of the next is never printed.

The shipped overlay polls `log 0`, which is `BC250_ESCAPE_GET_LOG` and writes no line at all. It
asks for a summary only for the `graphics.summary` action of its table.

## How to read the ring on the lab

```
bc250kmd_cli log                 the whole ring
bc250kmd_cli log <sequence>      from that sequence on
bc250kmd_cli log summary         the counter block into the ring, then the whole ring
bc250kmd_cli log summary only    the counter block alone, and no ring line but one
```

The first line of the output states the lines logged since the driver load, the lines lost to the
wrap, and the lines dropped above `DISPATCH_LEVEL`. A sequence number that starts at 0 again proves
that the driver unloaded.
