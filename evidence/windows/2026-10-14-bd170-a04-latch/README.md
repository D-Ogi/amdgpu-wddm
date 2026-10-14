# BD-170 a04: when an immediate flip has latched (2026-10-14, unit A)

A diagnostic lab arm of BD-170 on Windows 11 23H2 (22631), 16:43-16:55Z: the r5b KMD with the a04 diagnostic patch
(lab flavour only, SYS `6F6B36CA...`), and the released KMD `B0E01A86...` restored and loaded at the end. It
measures which hardware witness of an immediate flip holds in the DDI call, and when. The fact that it measures
is M903 in [`docs/facts/display.md`](../../../docs/facts/display.md). The design that uses it is
[`docs/design/vidpn-flip.md`](../../../docs/design/vidpn-flip.md), section 7.2.

## Files

| File | What it is |
|---|---|
| `steps.txt` | The arm's steps with UTC times, the restore and the loaded KMD hash. |
| `a04-report.txt` | The per-trial histograms, made from the KMD summaries before and after each trial. |
| `06.10-flips.txt` | The KMD summary at the end of the arm (`bc250kmd_cli`), with the last records of the ring. |

Nothing was removed from these files.

## What the diagnostic did

For every immediate flip (`FlipImmediate`) the diagnostic programmed the flip and then sampled in the call, 1 us
apart, until P' held: `SURFACE_FLIP_PENDING` clear, `PRIMARY_SURFACE_ADDRESS` equal to the request, and the
primary generation unchanged. Each sample read, in this order: the scan position, the flip event bits, pending,
the frame count, `PRIMARY_SURFACE_ADDRESS`, `SURFACE_INUSE` high word first and low word first, and
`SURFACE_EARLIEST_INUSE`. After the call it sampled the same witnesses at each vblank. The witnesses are:

| Name | Witness |
|---|---|
| P | `SURFACE_FLIP_PENDING` first observed clear |
| A | `PRIMARY_SURFACE_ADDRESS` reads the request |
| Q | P' (P and A, generation kept) |
| H, L | `SURFACE_INUSE` reads the request, high word first or low word first |
| E | the flip event (`SURFACE_FLIP_OCCURRED`) of this request |
| W | the flip-away event |
| R | `SURFACE_EARLIEST_INUSE` reads the request |

Trials: x64 exclusive fullscreen (a04-1), x64 with `-mk` and a camera burst (a04-2), WoW64 exclusive fullscreen
(a04-3), the D3D12 frameloop set `bd170`, and vkcube over DXGI with IMMEDIATE then FIFO. Each trial took less than
three minutes.

## Result

- 116 225 immediate flips were sampled, and P' held in the call for all of them: 612, 628, 612, 62 341 and
  52 032 in the five trials. 4 022 of them were programmed in the vertical blank, and all eight bins of the active
  scan have flips with P' in the call. No flip got `STATUS_DEVICE_BUSY`, and no read failed.
- 116 224 of them qualify for the witness histograms. One had the pending bit set when it was armed and is left out
  of that comparison.
- For those 116 224, P, A, Q, E, W and R have the same first-observation bucket for every flip, and none of them is
  `never`. The buckets, in microseconds after the start of the programming: under 1 us 87 719, 1-2 us 28 486,
  2-5 us 16, 5-10 us 2, 10-20 us 1. H and L never read the request.
- The 262 next-VSync controls: A matches already in the call (106 in the under 1 us bucket, 156 in 1-2 us). P, Q,
  E, W and R are never seen in the call, and are first seen at the first vblank after the call. So the readback of
  the programmed address alone does not show that a flip has latched.
- The largest call time of the report (18 us for immediate flips, 76 us for next-VSync controls) is the start time
  of the last sample, counted from the start of the programming. It is not the duration of the DDI.

## Limits

- One timestamp is taken before the reads of a sample and applies to all of them, so equal buckets do not show the
  order of the witnesses inside a sample.
- The diagnostic bound was a polling threshold: it checked P' before the time, and derived a late answer from the
  first sample only. So `late 0` does not show that a deadline was enforced.
- The diagnostic kept two r5b branches that answered success without P' (an exclusive request, and no live
  VSync). All sampled outcomes were P', so these branches did not act in this arm. r6 removes them.
- The data cover address-only flips on a running, unblanked pipe with one writer. They do not cover a format or
  pitch change, a stopped or blanked pipe, two writers, or a flip that was pending when it was armed.
