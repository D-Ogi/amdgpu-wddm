# BD-170 a03: a busy answer holds the flip queue (2026-10-14, unit A)

A lab arm of BD-170 on Windows 11 23H2 (22631), KMD candidate r5b (SYS `13D1D2BE...`), 14:17-14:36Z. The arm
installed the candidate, ran DXGI clients that flip at interval 0 and 1, and restored the released KMD
`B0E01A86...`. It is the check of fact M898 on 22631. The fact that it measures is M902 in
[`docs/facts/display.md`](../../../docs/facts/display.md). The design that uses it is
[`docs/design/vidpn-flip.md`](../../../docs/design/vidpn-flip.md), section 7.2.

## Files

| File | What it is |
|---|---|
| `steps.txt` | The arm's steps with UTC times, the restore problem and the recovery restart. |
| `06.2-flips-after.txt` | The KMD summary after trial a03-1 (`bc250kmd_cli`), with the four immediate flip records. |
| `dxgkrnl-timeline.txt` | Rows of the trial's `dxgkrnl.etl`, extracted with the xperf dumper: `MMIOFlip`, `Flip`, `CancelFlip`, `ClearFlipDevice` and runs of `VSyncInterrupt` with the same scanned address merged. |

The ETL itself and the client logs stay outside the repository (size). Nothing was removed from these files.

## What r5b did

r5b answered an immediate flip (`FlipImmediate`, DDI flags `0x002`) with success only when
`SURFACE_FLIP_PENDING` was clear and `SURFACE_EARLIEST_INUSE` named the requested surface, both read in the call,
and with `STATUS_DEVICE_BUSY` otherwise.

## Result

- Trial a03-1 sent 4 immediate flips. All 4 records have the outcome `old-in-use`: the pending bit was clear at the
  first sample (5 to 7 us after the programming started) while `EARLIEST_INUSE` named the old surface at the last
  sample (56 to 58 us). All 4 got `STATUS_DEVICE_BUSY`. The counters read `4 busy, 0 retried`.
- The first of them is `MMIOFlip ... flags 2` at 2 489 304 us in the timeline. From 2 491 830 us on, every
  `VSyncInterrupt` row reports the requested address as scanned: 613 reports in 8.2 s. The OS did not execute
  the busy head again in that time. `CancelFlip` came at 10 658 071 us, together with `ClearFlipDevice`, when the
  client process went away. The timeline has 4 `MMIOFlip` rows with flags 2, one for each busy flip: the OS
  called the DDI once for each of them and never again.
- Until then, the busy head held every flip of source 0, the composed desktop included. The client stalled in
  Present. At the end of the arm the restore of the released KMD failed, because the stuck client still held the
  device. A restart ended the client, and a second restore and restart loaded `B0E01A86...` again (steps 12 to 15).
- The KMD's own-vblank branch (`immediate own-vsync`) counted 0, so this trial does not exercise it.

## Limits

One trial, one OS build, four flips. The times in the flip records are first-observation times: each sample takes
its timestamp before its register reads. They do not measure the edge of the pending bit. The timeline is an
excerpt; it is not a full scheduler trace.
