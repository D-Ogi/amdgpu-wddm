# Research notes

Desk research for a coming milestone, kept as delivered. Everything in here is derived from
source code and documentation (kernel, libdrm, WDK headers, Microsoft Learn), with citations; **nothing in here is a
measurement on our hardware**. What turns out to hold on unit A becomes a fact (`docs/facts/data/`) with its evidence; decisions
taken from these notes become ADRs. Since M9 the directory also holds acceptance indexes and audits: they cite facts
and evidence, they do not replace them, and the deployed state is always workspace `STATE.md`.

Two kinds of note here break the rule above, and each says so in its own first lines. An investigation record
carries measurements, and its measured rows are facts. A literature survey quotes its sources word for word.

The eight `h264-encoder-literature-*` files are a literature survey of 2026-10-06, landed as delivered. Every
quotation in them is verbatim and no rewrite may touch one. The four surveys each have an independent re-check
beside them, the re-check is newer than the survey, and some verdicts are VERIFIED WITH CORRECTIONS that the
survey text does not carry. Read the check before you act on a quote. These eight files still owe the
ASD-STE100 pass that the rest of the documentation gets.

| File | For | State |
|---|---|---|
| `m6-compute-dispatch.md` | The compute dispatch that closes M6: libdrm's gfx10 memset shader and its 18 PM4 packets | Input to the shim's dispatch emitter; M6 closed |
| `m7-full-wddm-miniport.md` | M7: from the display-only miniport to a full WDDM miniport that a Vulkan ICD can submit to; staged plan that keeps the failsafe | Input to ADR 0008; M7 built in its stages |
| `m9-acceptance-status.md` | M9: requirement index for the DMA contract audit and the startup plan, reviewed through M471 | Current index; M9 open |
| `m9-acceptance-history-through-m447.md` | Earlier snapshot of the same index, reviewed through M447 | Historical, superseded by `m9-acceptance-status.md` |
| `m9-dma-contract-audit.md` | M9: WDDM paging builder, physical/virtual submission, private UMD payload, completion and the CPU Present path, checked against the local WDK 26100 contract, with each candidate's results | Working audit; historical entries do not prove current acceptance |
| `m9-system-paging.md` | M9: the remaining system-memory paging design and implementation work | Incomplete design note, 2026-09-23 |
| `driver-review-2026-09-24.md` | How the three-reviewer driver review of 2026-09-24 was integrated; statuses live in workspace `DEFECTS.md` | Integration pointer |
| `bios-analysis-followup.md` | What the BIOS static analysis (workspace `firmware/bios/analysis`) means for the driver, reviewed against facts M430-M435 | Review note, 2026-09-24 |
| `m13-present-baseline.md` | M13: the working full-WDDM desktop baseline on the CPU renderer and its controls | Status note, checkpoint M406-M412; later steps indexed in workspace `STATE.md` and `facts.md` |
| `offgpu-frame-cost-c48-c55.md` | M15 off-GPU frame cost: what the long idle gaps of the 3D ring are. Which five explanations died. Why our own overlay poll made the class. The rules that come out of it | Investigation record, 2026-10-06. Its measured rows are facts M797 to M800 |
| `k137-kmd-stop-cpu-cap.md` | BD-093: why a KMD stop leaves the CPU slower until a Windows restart. The constraints, five hypotheses and what killed each one, the next tests | Investigation record, 2026-10-07. Its measured rows are facts M832 to M834 |
| `gfx1013-isa-leads-anyps5.md` | M16: the 36 opcode slots that the AnyPS5 PS5 shader decoder knows and LLVM refuses for gfx1013, and other ISA leads from that project | Leads for the gfx1013 sweep, 2026-10-10. Nothing measured on unit A |
| `h264-encoder-literature-motion-estimation.md` | M15.11: what the literature does for GPU motion estimation, against our `cs_me.hlsl`. Ranked levers | Literature survey, 2026-10-06. Check beside it. STE pass owed |
| `h264-encoder-literature-motion-estimation-check.md` | The independent re-check of that survey, source by source | Check record, newer than the survey |
| `h264-encoder-literature-mode-decision.md` | M15.11: mode decision and rate-distortion choice on the GPU, against our `cs_mb.hlsl` | Literature survey, 2026-10-06. Check beside it. STE pass owed |
| `h264-encoder-literature-mode-decision-check.md` | The independent re-check of that survey | Check record, newer than the survey |
| `h264-encoder-literature-entropy-pipelining.md` | M15.11: entropy coding and how to overlap it with the GPU passes, against our CPU CAVLC | Literature survey, 2026-10-06. Check beside it. STE pass owed |
| `h264-encoder-literature-entropy-pipelining-check.md` | The independent re-check of that survey | Check record, newer than the survey |
| `h264-encoder-literature-rate-control.md` | M15.11: rate control and perceptual quality, which is where our measured quality gap against the inbox encoder sits | Literature survey, 2026-10-06. Check beside it. STE pass owed |
| `h264-encoder-literature-rate-control-check.md` | The independent re-check of that survey | Check record, newer than the survey |
