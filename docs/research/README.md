# Research notes

Desk research for a coming milestone, kept as delivered. Everything in here is derived from
source code and documentation (kernel, libdrm, WDK headers, Microsoft Learn), with citations; **nothing in here is a
measurement on our hardware**. What turns out to hold on unit A becomes a fact (`docs/facts/data/`) with its evidence; decisions
taken from these notes become ADRs. Since M9 the directory also holds acceptance indexes and audits: they cite facts
and evidence, they do not replace them, and the deployed state is always workspace `STATE.md`.

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
