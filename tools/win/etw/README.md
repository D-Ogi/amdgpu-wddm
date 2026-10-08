# tools/win/etw - present mode and flip evidence from a DxgKrnl capture

`etw-present-mode.py` says how each present reached the screen. The desktop compositor composed it, or the
display pipeline scanned out the application's own buffer as an independent flip. The tool is the acceptance
instrument of M15.14, which asks that a fullscreen or borderless game scans out its own swap-chain buffers. It
also carries the two named verdicts of the DirectFlip route decision, one for increment 1 and one for
increment 2.

```
python etw-present-mode.py TRACE.etl [PROCESS ...] [options]
python etw-present-mode.py DUMP.txt [PROCESS ...] [options]
python etw-present-mode-test.py                        # the self-test: no lab, no capture of its own
```

`TRACE.etl` is the capture of a present-mode session. `scratch/train/b18r1-iflip-etw.ps1` and
`tools/win/lab-runner/etw/` start that session. The tool turns the `.etl` into text with the xperf dumper. This
is the same xperf path that `scratch/m15/gpu-timeline/align/etwclock.py` and `etwdump.py` use. **The tool then
deletes the text dump**, unless you give `--keep-dump`. A dump of a 44 MB trace is 51 MB, and P: filled up once
on exactly these files. The tool reads a `.txt` argument as an existing dumper text, and it never deletes that file.

`tracerpt` is not a substitute. Its CSV is a different format. Evidence
`evidence/windows/2026-09-27-E34-dwm012-artifacts` records what happens when somebody feeds it to an xperf
parser.

`PROCESS` arguments limit the per-present tables and every verdict line. Each one is a substring match on the
dumper's process column. A session without the kernel image rundown writes that column as `"Unknown" (4476)`,
which is the normal shape of these captures. Match on the pid there.

## The two sides, reported apart

The tool reports two independent answers side by side. It never merges them into one number, because no field
in this provider set joins a present-history token to a submit sequence.

- the compositor's answer, from the Win32k token states and the Dwm-Core consumption and surface updates.
  This is the side that decides whether a flip may happen at all.
- the kernel's answer, from the client's own present packets and the DxgKrnl `IndependentFlip`, `MMIOFlip` and
  VSync DPC events that name their submit sequences. This is the side that says what happened to the client's
  own buffer.

When the two disagree, that disagreement is the finding. The composed baseline says why the tool needs both. DWM
answered `bDirectFlip 1` for 600 presents and then consumed all of them itself.

## The verdict lines

A trial greps four lines instead of reading the tables.

| Line | What it answers | Values |
|---|---|---|
| `M15.14 VERDICT` | the per-frame counts, and the compositor's DirectFlip answer | `INDEPENDENT-FLIP`, `COMPOSED`, `NO-DATA` |
| `M15.14 KERNEL-WITNESS` | whether the kernel's own events prove that the process's buffers went to the plane, so that the Win32k skip label does not apply | `HOLDS`, `NOT-HELD`, `NO-DATA` |
| `M15.14 INCREMENT1` | the inert control: the front asks the DirectFlip question and answers FALSE, so the capture must still equal the composed baseline | `INERT`, `MOVED`, `NO-DATA` |
| `M15.14 INCREMENT2` | the four-clause conjunction, because DirectFlip without independent flip has no unique ETW event | `CONFIRMED`, `REFUTED`, `UNKNOWN` |

Each clause prints its own `PASS`, `FAIL` or `NO-DATA` line with the numbers behind it. A clause with no input
reads `NO-DATA` and keeps the whole verdict away from `CONFIRMED`. Three clauses out of four are not a result.

Read `INCREMENT1` for the client under test only. On a composing desktop the compositor's own packets are the
ones that reach the plane, so the compositor reads `MOVED` by construction. The line says so itself: for
`dwm.exe` it carries a `CAVEAT` that names the reason, so nobody reads that `MOVED` as a finding.

Every clause of the control is about this process. A capture holds the whole desktop, so an independent flip
of another process is a count on the clause line and not a verdict about the client: a trace-wide test would
send a lead rolling back a route that never moved.

## The kernel witness

Trial 478 (The Witcher 3, D3D12, 2026-10-08) gave a shape that the first rules read wrongly. Win32k marked every
InFrame token of the game with `IndependentFlip` true and `SkipIndependentFlip` true. The rules read that pair
as "composed flip (independent skipped)". The kernel events of the same frames said the opposite. Each present
packet became an `IndependentFlip` and an `MMIOFlip`. The VSync DPC scanned the same three addresses. DWM consumed
none of the presents.

`KERNEL-WITNESS` is the rule for that shape. It has four clauses for one process:

| Clause | `PASS` when |
|---|---|
| `independent_flips` | at least one present packet of the process became an `IndependentFlip` |
| `mmio_programmed` | at least one of those flips reached `MMIOFlip` |
| `vsync_scanned_same_addresses` | a VSync DPC scanned every physical address those flips programmed |
| `not_consumed_by_dwm` | DWM consumed no present of the process, and every consumption row decoded |

`HOLDS` needs all four. Then, and only then, a present with the skip pair and with no consumption reads as
`hardware flip (kernel witness; Win32k skip)`. The witness moves no other label. A present that DWM consumed stays
composed, and a present without a token state stays unclassified. One `FAIL` gives `NOT-HELD`, and a missing input
gives `NO-DATA`. Neither of them moves a label. `relabelled` on the line counts the presents that moved.

The witness is per process and per capture. One consumed present of the process stops the relabel for all of its
presents in that capture. That is the safe direction: a capture that mixes a composed segment with a flipping
segment reads as composed, and the per-second table still separates the two segments.

`INCREMENT2` reads two of its clauses from numbers outside the trace:

```
--admitted-address 0x271001000      a physical address the kernel driver logged as an admitted scan-out flip
--kmd-counters scanout_flips=3,scanout_requests=3,admit_alignment=0,admit_segment=0
```

The counter names are the spellings of `bc250kmd_cli log summary`, which `tools/win/d3d12queue/scanout-trial.ps1`
already reads. Clause 4 needs `scanout_flips`, `scanout_requests` and at least one `admit_*` refusal column.

## What the tool cannot answer

It is not PresentMon and does not reproduce the PresentMon state machine. A present that the rules cannot place
counts as unclassified, with the shape that made it so. The tool never guesses a present into a class.
Field positions come from the dumper's own header block, so no column index is hard-coded. The tool counts and names
every row of an event kind with no header line. Such a row reads as all-None and would otherwise vanish
without a word. A LUID column reads as one number or as the dumper's expanded `{lowpart; highpart}` struct,
whichever the manifest gives. The tool counts a consumption row whose surface key did not read at all, and
fails the clause that asks whether the compositor consumed the present. An absence that comes from a field nobody
decoded is not evidence of an absence.

The tool cannot say which plane a multi-plane overlay flip belongs to, beyond `LayerIndex`. It cannot tell
exclusive fullscreen from borderless. Both answers lie outside the increment of this parser.

The module docstring holds the full rule list. Each rule names the event that carries it.

## The unconfirmed flag bits

`MMIOFlip` Flags has three bits that PresentMon's generated model of this provider names: `ModeChange` 0x1,
`FlipImmediate` 0x2 and `FlipOnNextVSync` 0x4. The DDI word `DXGK_SETVIDPNSOURCEADDRESS_FLAGS`
(`d3dkmddi.h:6212`) continues with `SharedPrimaryTransition` 0x20, `IndependentFlipExclusive` 0x40 and
`MoveFlip` 0x80. M15.14 cares about the first two of those three.

No source states that the event's Flags field is that full DDI word. PresentMon's model stops at 0x4, and
all 718 flips of the composed baseline read 4 under either mapping. The tool therefore prints the three high
bits with a trailing `?`, and a run that sees one of them prints a warning line. These bits corroborate. The
kernel driver's own counters stay the witness.

## Provenance

The event and field names of `IndependentFlip` (SubmitSequence, FlipInterval, keyword `0x4000000000000001` on
the DxgKrnl Performance channel 0x11), `MMIOFlip` (FlipSubmitSequence, Flags) and `QueuePacket` (SubmitSequence,
bPresent) come from PresentMon's generated model of this provider: `ref/presentmon-etw`,
`PresentData/ETW/Microsoft_Windows_DxgKrnl.h` and `PresentMonTraceConsumer.cpp`, MIT. `D3DKMT_PRESENTFLAGS`,
`D3DKMT_PRESENT_MODEL` and `DXGK_SETVIDPNSOURCEADDRESS_FLAGS` come from the WDK headers in `toolchain/nuget`,
10.0.26100. The Win32k `TokenState` values come from PresentMon's
`ETW/Microsoft_Windows_Win32k.h`.

## The self-test

`etw-present-mode-test.py` runs on the development PC and needs no capture of its own. `tools/quality/quick.ps1`
runs it as the `present-mode` gate. It uses these fixtures:

- `testdata/base-composed-excerpt.txt`, one second of the composed baseline. Every shape in it is a real shape
  of this driver. Increment 1 must read `INERT` on it, and increment 2 must read `REFUTED`.
- `testdata/w3-exclusive-flip-excerpt.txt`, one second of trial 478 in exclusive fullscreen at the native mode.
  The kernel witness must read `HOLDS` and relabel 102 presents.
- `testdata/w3-exclusive-1080-composed-excerpt.txt`, one second of trial 478 at 1920x1080, where DWM composed the
  game. The kernel witness must read `NOT-HELD`, and no label may move.
- dumper texts the test writes itself, in the real header shapes, for the branches that no capture of this
  driver took yet. A hardware independent flip is one of them. The negative controls of the kernel witness are
  others: one consumed present, a DPC that scanned other addresses, and no `IndependentFlip` event.

Set `BC250_ETW_TEST_ETL` to a present-mode `.etl` to also exercise the xperf path and the dump deletion. The
test skips that case when the variable is absent, because no `.etl` belongs in this repository.
