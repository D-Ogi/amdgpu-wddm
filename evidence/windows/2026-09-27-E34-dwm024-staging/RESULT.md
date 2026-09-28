# DWM024 staging

2026-09-27. Three-minute hosted DWM interop shape-capture runner, not launched
at08:51:57Z. Exact KMD1608E676C81; hosted UMD5C74BF98/ICD3508416F.
Router and GDI animation compile with /W4 /WX. On-target PS5 parsing and all
manifest hashes pass. Exact159 rollback remains available.
The no-pending-marker restore path returns without changing interop0.

Runner starts an independent300-second watchdog before changing libraries,
latches interop1 through an adapter restart, refreshes adapter LUID, then
permits the router's one-DWM-process claim. Restore recovers baseline DLLs,
latches interop0 through adapter restart and restarts DWM. A persistent pending
marker identifies partial changes; one named mutex serializes gate mutations.
GPU Present stays0 throughout this shape-capture stage.
The enabled and rollback transitions are not runtime-tested by staging.
This is not an interop or G0 acceptance result.
