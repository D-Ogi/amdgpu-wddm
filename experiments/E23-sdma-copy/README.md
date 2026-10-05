# E23 - SDMA copy and fill as a positive control (stage D, ADR 0013)

State: **run 001 done** (facts M95). Next: the paging node in the full table.

## Why

ADR 0013 makes SDMA the paging node: `BuildPagingBuffer` will fill SDMA copy/fill packets and `SubmitCommand` on
node 1 will run them. Before the table is touched, the packets and the fence must be proven on the hardware from
the bring-up path we already trust (E15, facts M53-M60): fill a source, copy it, fence, read the destination back by
CPU. Nie od razu Kraków zbudowano (Krakow was not built in a day) - the node comes after the copy.

## Build

bc250kmd 0.7.21/0.7.22: `driver/shim/bc250_sdma_copy.c` emits the `SDMA_OP_COPY`/`COPY_LINEAR` and
`SDMA_OP_CONST_FILL` packets of amdgpu's `sdma_v5_0_emit_copy_buffer` / `sdma_v5_0_emit_fill_buffer` (split at
0x400000 bytes), host test `driver/shim/test/sdma_copy_packets.c` (38 checks); escape `BC250_ESCAPE_RUN_SDMACOPY`
/ `bc250kmd_cli sdmacopy [bytes]`: two 64 KiB VRAM regions from the bring-up's VRAM pool, CPU seed (source: counting
bytes, destination: 0xEE), SDMA0 fill(source, 0xA5) -> copy(source -> destination) -> fence (slot 2, the fence
escape's bookkeeping), CPU read-back compared byte by byte. Needs the EnableGfx chain to stage 7 and EnableVramWrite,
not the IH ring (the fence is polled). The seven SDMA shim defects of the fault-injection suite (D-01..D-07,
`driver/shim/test/sdma_faults_report.md`) were fixed first.

## Hypotheses

- H1: the packets transcribed from sdma_v5_0 run on SDMA0 as emitted and the fence after them retires ->
  **confirmed** (fence 1 and 2 read back, M95).
- H2: the destination holds the fill's pattern after the copy, i.e. both packets did their work (a dead fill would
  have copied the counting seed instead) -> **confirmed**, 4096 and 65536 bytes every byte 0xA5 (M95).
- H3: the bring-up with the SDMA work in the middle undoes cleanly (gfx fini, psp unload, gart restore) and the
  display-only driver keeps presenting -> **confirmed** (presents kept counting, gates closed, no event).

## Runs

| Run | Build | What | Result |
|---|---|---|---|
| 001 | 0.7.22 (4c233e3) | fresh restart, gate on, gart, psp, gfx run 7, ring-test fence, `sdmacopy 4096`, `sdmacopy 65536`, full undo | both MATCHED, 11-12 us seed to read-back, 69 C. M95. `evidence/windows/2026-09-22-E23-sdma-copy-run-001/` |
