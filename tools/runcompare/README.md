# tools/runcompare - what changed between two lab runs

M7 stage A is run over and over with exactly one thing different: a package, a gate, an
answer in `wddm.c`. Every run leaves a driver log ring and a handful of state files from
the target script, and the question afterwards is always the same one - how far did
dxgkrnl get this time, and where exactly did this run stop agreeing with the last one.

Done by eye that means reading two 35-line logs side by side and trusting yourself not
to skip a line, which is how "it behaved the same as last time" gets written into a
journal about a run that did not. This tool answers it in one sentence and shows its
working:

```
First difference: after QueryAdapterInfo type 1 (DRIVERCAPS, answered) run B went on to
QueryAdapterInfo type 15 (PHYSICALADAPTERCAPS) -> STATUS_NOT_SUPPORTED;
run A stopped (stage 70).
```

It reads. It writes nothing into a run directory, and `evidence/` is immutable.

## Usage

```
python tools/runcompare/runcompare.py show <run-dir-or-log>
python tools/runcompare/runcompare.py diff <runA> <runB> [--json] [--markdown]
python tools/runcompare/runcompare.py normalize <log>
python tools/runcompare/runcompare.py --tables
python -m unittest discover -s tools/runcompare
```

A **run** is either an evidence directory - `evidence/windows/2026-09-21-E16-run-003`,
ring logs plus `gate-x-*.txt` / `state-*.txt` / `install-x-*.txt` - or a single ring log.

`show` prints, for one run: the driver version out of the log header, the package and
`DriverVer` and whether a `UserModeDriverName` is in the software key, the gates, the
ordered PnP/DDI sequence after the last `stage 10`, every `QueryAdapterInfo` with its
type name, input and output size and NTSTATUS, the `DXGK_DRIVERCAPS` answer field by
field, the object lifecycle (below), the last stage reached, the first refusal or error,
how long after `stage 39` (StartDevice returned) the stop came, any TDR line, and the PnP
problem code with its `CM_PROB_*` name.

`diff` prints the sentence, then the two sequences aligned next to each other, then the
object lifecycle of both, then the facts that differ. A fact that was compared and
matched is not a row; it is named in the "compared and equal" line, so that a short list
means "checked" and not "not looked at".

`normalize` prints the normalized log alone, for committing next to the evidence or for
feeding to a plain `diff`.

### Which ring, which state file

A run directory usually holds two rings: the display-only instance that the device
disable stopped, and the full-table instance under test. `runcompare` takes the one whose
log carries `gate: EnableFullWddm 1`; `--ring <file>` overrides it, and `show` lists the
others it saw.

The state files are the same story - the directory holds the state before the gate was
opened, the state at the failed start, and the state after the gates were closed again
and the display-only driver came back. The one paired with a full-table ring is the one
whose `gates` line has `EnableFullWddm 1`; otherwise the last one written. `--state`
overrides it.

## The object lifecycle section

As far as any E16 run has got, dxgkrnl builds a system process, one or two devices and a
context on the adapter, takes all of it down again and stops the device - run 004 did the
whole thing in 2 ms. Those few milliseconds are the only evidence there is about what
dxgkrnl wanted, so `show` and `diff` unpack them:

- each `Create*` and `Destroy*` in order, with the gap from the previous one in
  milliseconds, taken from the log's own seconds column;
- **whether creation and destruction are symmetric** - every object created destroyed
  again, nothing left alive at the stop - and whether the teardown ran in the reverse of
  the creation order;
- how long the whole object graph lived, first creation to last destruction;
- `CreateProcess`, `CreateDevice` and `CreateContext` flags decoded bit by bit
  (`flags 0x00000005 (SystemContext | VirtualAddressing)`);
- every field of the `CreateContext` answer: `DmaBufferSize`, `DmaBufferSegmentSet` (as
  segment ids, since it is a bit per segment), `AllocationListSize`,
  `PatchLocationListSize` and `Caps` bit by bit;
- **the first DDI of the next stage that the run never called.**

The bit names come from `d3dkmddi.h` through `gen_tables.py` -
`DXGK_CREATECONTEXTFLAGS`, `DXGK_CONTEXTINFO_CAPS`, `DXGK_CREATEDEVICEFLAGS`,
`DXGK_CREATEPROCESSFLAGS` - never from memory, and a bit the header does not name is
printed as `bit N` rather than dropped. The generator takes the newest branch of each
`#if DXGKDDI_INTERFACE_VERSION`, because the value being decoded is the one dxgkrnl
passed and dxgkrnl is the newest; the driver itself compiles at
`DXGKDDI_INTERFACE_VERSION_WDDM2_0`, where `SystemProtectedContext` and everything above
it is still part of `Reserved`.

### The expectation table

"The first DDI of the next stage that the run never called" comes from
`EXPECTED_AFTER_SYSTEM_CONTEXT` in `runcompare.py`: six entries, each with the reason it
is there. It is small and meant to be edited.

**It has been observed on no run yet.** No E16 run has got past `DestroyProcess`, so the
list is a reading of the DDI table `driver/kmd/wddm.c` builds and of the WDK, not a
measurement, and every line that prints a name from it says so. When a run does get
further, correct the list from what that run shows and set `EXPECTED_ORDER_OBSERVED` to
that run's id; the wording changes by itself.

Two things the log cannot give this section: it carries no object handle, so creations
and destructions are paired by kind and by count and never one named object to another;
and the driver logs only the first `LOG_CALLS` (8) calls of each DDI, so where the stop
summary counted more than the log shows, `show` says so instead of quietly undercounting.

## Normalization rules

`diff` lines up the two sequences on a normalized form of each line, so that two runs
that did the same thing at different times and different addresses line up. What is
dropped, and what is not:

| dropped | kept |
|---|---|
| the sequence number and the timestamp of every line | every DDI name and the order they came in |
| the header's line count (`<n> lines`) | `lost to the wrap` and `dropped above DISPATCH_LEVEL` - they say whether the record is complete |
| addresses: the BAR5 base, the VRAM and MC bases, a segment's GPU and CPU address | sizes, strides, offsets and counts, including the VRAM and BAR0 sizes and the segment size |
| any hex literal of 9 digits or more in a line no rule knows (a 64-bit pointer) | flag words, caps masks, status codes, engine and node numbers |
| the wording and field list of the `DRIVERCAPS` line | the size it was written into, and each field, compared key by key as a fact |
| the column padding of the stop summary, and its `first at <seq>` numbers | the summary's counters |
| file-name times, when a name is printed | |

Two rules deserve their reason spelled out.

**The `DRIVERCAPS` line takes part in the alignment only by its size.** 0.7.3 wrote
`DRIVERCAPS 576 of 576 bytes: ... paging node 0 flip 0x2 slots 0` and today's `wddm.c`
writes `DRIVERCAPS into 576 bytes: ... flip 0x12 slots 0 tdr 1 dflip 1 rot 1`. Aligning
on the text would make every comparison across those builds report the caps line as the
first difference and hide what dxgkrnl did afterwards - which is the thing the runs are
for. So the line aligns by size and its fields are compared key by key in the facts list,
where a field that one build does not print says so.

**The stop summary is not part of the sequence.** It is a report *about* the run written
inside `DxgkDdiStopDevice`, not a step of it, and its wording moves between builds (the
"no TDR" line gained `ResetEngine`). Its counters are read - objects, adapter info types,
TDR - and compared as facts; its lines do not align.

## Limits

- **The log only holds the first calls.** `wddm.c` logs the first few calls of each DDI
  and, for `QueryAdapterInfo`, every refusal among the first 256 (`WddmFirstCalls`,
  `WddmAnswersLogged`). So a call the summary counts and the log does not is normal, not
  a hole in the evidence; `show` prints both numbers when they disagree.
- **First difference means first difference in the log**, not the first difference in
  what the driver did. Two runs that differ only in something nobody logged look equal.
- **A run with no state file** (a bare ring log) has no package, no gates from the
  registry and no PnP problem code. Those facts read `not recorded (no state file in this
  run)`, which is not the same as "equal".
- The tool names nothing from memory. A `QueryAdapterInfo` type outside the Kit's enum
  and an NTSTATUS outside the generated table are printed as their raw numbers.
- The alignment is `difflib.SequenceMatcher` over the normalized lines. Where a run
  repeats a line many times (four `type 14` calls in a row) the matcher may pair them
  differently than a human would; the first difference is unaffected, the side by side
  can look odd.

## The name tables

`tables_generated.py` is checked in and carries the Kit version it came from. It holds
`DXGK_QUERYADAPTERINFOTYPE` and the four flag bit fields from the WDK's `d3dkmddi.h`, the
handful of `NTSTATUS` values a miniport deals in from the SDK's `ntstatus.h`, `CM_PROB_*`
from `cfg.h`, `BC250_STAGE` from `driver/kmd/bc250kmd.h` and `BC250_WDDM_LOG_CALLS` from
`driver/kmd/wddm.c`. Regenerate it after a Kit or driver header change:

```
python tools/runcompare/gen_tables.py
python tools/runcompare/gen_tables.py --check     # what the test does
```

(The repository spells generated files `*.generated.*`; Python needs an importable module
name, so here the dot is an underscore.)

## Tests

`test_runcompare.py` runs on short synthetic logs written into the test - for the parsing,
normalization and lifecycle rules, which are easier to pin down when the log holds
nothing else - and on the real files of E16 runs 002, 003 and 004, read only. The two
first differences it pins down are the ones the experiment turns on:

- 002 -> 003: after `DRIVERCAPS`, run 003 is asked type 15 where run 002 was stopped;
- 003 -> 004: after type 47, run 004 goes on to type 13.

An earlier version of this tool carried a hand-written fixture for run 004, which had not
been captured yet. It is gone: `evidence/windows/2026-09-21-E16-run-004/` is the real
thing, and it differs from the fixture in enough fields (`type 13 in 4 out 24` and not
`in 0 out 32`, segment stride 104 and not 48, both `CreateDevice` calls with
`SystemDevice`) to be a decent argument against keeping invented evidence around.
