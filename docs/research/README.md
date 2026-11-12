# Research notes

Desk research for a coming milestone, kept as delivered. The notes come from source code and documentation
(kernel, libdrm, WDK headers, Microsoft Learn), with citations. **A note here is not a measurement on our
hardware**, except for the investigation records below. What turns out to hold on unit A becomes a fact
(`docs/facts/data/`) with its evidence. A decision that comes from these notes becomes an ADR. The deployed state
is always the workspace `STATE.md`.

An investigation record is the exception to the rule above, and it says so in its first lines. It carries
measurements, and its measured rows are facts.

The finished records of earlier work are in [`docs/archive/research/`](../archive/research/). These are the M6,
M9, G0 and M13 notes, the reviews of 2026-09 and the H.264 encoder literature survey of 2026-10-06. The
[archive README](../archive/README.md) lists them.

| File | For | State |
|---|---|---|
| `offgpu-frame-cost-c48-c55.md` | M15 off-GPU frame cost: what the long idle gaps of the 3D ring are. Which five explanations died. Why our own overlay poll made the class. The rules that come out of it | Investigation record, 2026-10-06. Its measured rows are facts M797 to M800 |
| `k137-kmd-stop-cpu-cap.md` | BD-093: why a KMD stop leaves the CPU slower until a Windows restart. The constraints, five hypotheses and what killed each one, the next tests | Investigation record, 2026-10-07. Its measured rows are facts M832 to M834 |
| `bd180-time-spy-device-loss-and-timeout.md` | BD-180 and GitHub issue #38: why 3DMark Time Spy loses its device, and the GPU timeout that stays after that fix. Rounds A1 to A28, the arm that removes our D3D12 layer, the held engine commit, the open questions | Investigation record, 2026-10-14 to 2026-10-21. Its measured rows are facts M911 to M915 |
| `gfx1013-isa-leads-ps5-decoders.md` | M16: the opcode slots that the PS5 shader decoders of AnyPS5, KytyPS5 and sharpemu know and LLVM refuses for gfx1013. An image opcode that a PS5 game uses and nobody names. Other ISA leads | Leads for the gfx1013 sweep, 2026-10-10. Nothing measured on unit A |
| `ser-reordering-literature.md` | SER stage 2 on gfx1013: ray and shader-invocation reordering in software, with the measured results of each source in short form | Literature list, 2026-10-23. No quotations |
