E20 runs 001 and 002, unit A, 2026-09-22 (boot 01:40:28), Windows 11, full WDDM table behind its one-shot gate.

run-001-console.txt  bc250kmd 0.7.14 (sys sha256 4846802a0f444b6f...): CreateContext answers a 256-entry allocation
                     list to a GDI context. Console of e20_run1.sh: install with the gate closed, state, confirm,
                     gate open (Full 1, GpuVa 1), the driver's ring log, gate closed, state.
run-002-console.txt  bc250kmd 0.7.15 (sys sha256 6f14178c7ad49937...): Present logs the list pointer and the entries at
                     DXGK_PRESENT_SOURCE_INDEX (1) and DXGK_PRESENT_DESTINATION_INDEX (2) raw, three qwords each.

No engine was started in either run (no GART, PSP, ring or interrupt source). Log columns: line number, seconds since
the driver's start, text. Nothing redacted: the files carry no addresses, MACs, serials or SSIDs.
