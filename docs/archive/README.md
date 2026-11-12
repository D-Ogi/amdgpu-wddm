# Archive of finished records

This directory holds the records of finished work: research audits, literature surveys and their re-checks,
acceptance histories and dated audits. Each record keeps its sub-path below `docs/`. For example,
`docs/research/x.md` moves to `docs/archive/research/x.md`.

## Rules

- Nobody maintains these records. A record does not change after it moves here, except for its own relative
  links when the move breaks them.
- The style gate (`tools/quality/doclint.py`) does not read this directory, because a record is not rewritten.
  A baseline line for a path in this directory is an error of the gate.
- A record can be out of date. The deployed state is always the workspace `STATE.md`. The facts are in
  `docs/facts/data/`.
- A fact never cites a record here as its only source when a living document has the same information. Cite
  the living document, and cite the record as history.
- Move a record here with `git mv`. Change every reference to its old path in the same commit.

## The H.264 encoder literature survey

The eight `research/h264-encoder-literature-*` files are a literature survey of 2026-10-06, kept as delivered.
Every quotation in them is verbatim, and no change may touch one. Each of the four surveys has an independent
re-check next to it. The re-check is newer than the survey. Some verdicts are VERIFIED WITH CORRECTIONS, and
the survey text does not carry these corrections. Read the check before you act on a quote.

## Contents

| File | For | State when archived |
|---|---|---|
| `research/m6-compute-dispatch.md` | The compute dispatch that closes M6: libdrm's gfx10 memset shader and its 18 PM4 packets | Input to the shim's dispatch emitter. M6 closed |
| `research/m9-acceptance-status.md` | M9: requirement index for the DMA contract audit and the startup plan, reviewed through M471 | Last index, 2026-09-25 |
| `research/m9-acceptance-history-through-m447.md` | Earlier snapshot of the same index, reviewed through M447 | Superseded by `research/m9-acceptance-status.md` |
| `research/m9-dma-contract-audit.md` | M9: WDDM paging builder, physical/virtual submission, private UMD payload, completion and the CPU Present path. The audit checks them against the local WDK 26100 contract, with each candidate's results | Audit. Its historical entries do not prove current acceptance |
| `research/m9-system-paging.md` | M9: the remaining system-memory paging design and implementation work | Incomplete design note, 2026-09-23 |
| `research/driver-review-2026-09-24.md` | How the three-reviewer driver review of 2026-09-24 was integrated. The statuses are in the workspace `DEFECTS.md` | Integration pointer |
| `research/bios-analysis-followup.md` | What the BIOS static analysis (workspace `firmware/bios/analysis`) means for the driver, reviewed against facts M430-M435 | Review note, 2026-09-24 |
| `research/m13-present-baseline.md` | M13: the working full-WDDM desktop baseline on the CPU renderer and its controls | Status note, checkpoint M406-M412. Later steps are in the workspace `STATE.md` and `facts.md` |
| `research/g0-cpu-copy-coverage.md` | G0 CPU-copy evidence coverage after M679: a source-bound review and a plan for the measurement gaps | Review. The later G0 status is in `research/g0-implementation-status.md` |
| `research/g0-implementation-status.md` | GPU desktop G0: implemented and measured on unit A (DWM050/M723), audited by M724 | Status record |
| `research/2026-09-27-windowed-present-under-dwm.md` | Windowed present of a device-local image under DWM composition, for the E45 result (M598) | Research note, 2026-09-27, with its correction of the same day |
| `research/h264-encoder-literature-motion-estimation.md` | M15.11: what the literature does for GPU motion estimation, against our `cs_me.hlsl`. Ranked levers | Literature survey, 2026-10-06. Check next to it |
| `research/h264-encoder-literature-motion-estimation-check.md` | The independent re-check of that survey, source by source | Check record, newer than the survey |
| `research/h264-encoder-literature-mode-decision.md` | M15.11: mode decision and rate-distortion choice on the GPU, against our `cs_mb.hlsl` | Literature survey, 2026-10-06. Check next to it |
| `research/h264-encoder-literature-mode-decision-check.md` | The independent re-check of that survey | Check record, newer than the survey |
| `research/h264-encoder-literature-entropy-pipelining.md` | M15.11: entropy coding and how to overlap it with the GPU passes, against our CPU CAVLC | Literature survey, 2026-10-06. Check next to it |
| `research/h264-encoder-literature-entropy-pipelining-check.md` | The independent re-check of that survey | Check record, newer than the survey |
| `research/h264-encoder-literature-rate-control.md` | M15.11: rate control and perceptual quality, which is where our measured quality gap against the inbox encoder sits | Literature survey, 2026-10-06. Check next to it |
| `research/h264-encoder-literature-rate-control-check.md` | The independent re-check of that survey | Check record, newer than the survey |
| `testing/defaults-audit-2026-10-04.md` | What the tester.11 defaults do not do on the GPU, or do not enable for applications and games | Audit, 2026-10-04 |
| `testing/m16-hip-dispatch-cost.md` | Index of the historical M16 HIP performance arms and their corrected interpretation | Record index |
| `testing/m16-hip-step3.md` | Index of the historical M16 HIP step-3 trials: threads and application records | Record index |
| `predecessor-analysis.md` | Why the previous Windows driver attempt concluded "firmware-locked", and why that is most likely wrong | Desk analysis, 2026-09-21 |
| `prior-art-vs-hardware.md` | Prior-art claims compared with the measurements on unit A under Linux 6.18.52 | Comparison, 2026-09-21 (facts M1-M15) |
