# DWM039 checkpoint integration - NOT READY TO RUN

Hypothesis: M687's measured copy detector can bracket DWM rendering and isolate
primary/GDI capture reads on UMDCC82A2D9, preserving correct composition and GPU work.

Prepared from DWM037 at3ca3217; new directory and tasks039. Candidate Mesa0185cb8d,
hosted ICD3508416F, baseline KMD164/UMD8279/ICDCF39. Enable uploader and map markers
only in the selected DWM process. Each capture has separately acknowledged bounds.
The CPU audit checkpoint is not a GPU fence.

One clock starts before DLL/gate mutation. Planned render stop105s, final marker
limit130s, independent rollback140s, measured total acceptance<=180s. OS stalls
can exceed deadlines; a watchdog does not prove a hard real-time guarantee.

Local validation completed: router/control build with /W4 /WX,21 scripts parsed
under Windows PowerShell5.1,7 watchdog decision controls, and durable copy/restore
controls (including corrupt-backup fallback and preservation of unknown files).
Worker failures clear success; watchdog requires restoration evidence, including
when the worker already wrote done.json. Trial deadlines use shared QPC ticks.
Critical durable receipts publish by same-directory rename after Flush(true).
These are host controls, not a measured target watchdog/recovery exercise.

The local package has24 hashed files, manifest6D097845FF55DD3BFAABD7A44C62FFF0AD871C2B5FB77DE95BF51F6ACEA67208.
Router465F0219/control6B188424 are pinned by stage.py alongside the two DLLs;
compiled-source hashes must match the retained build receipt. Package is local,
not deployed. launch.ps1 deliberately refuses to run this reviewed-in-progress
snapshot. Remaining: review/install-abort fault coverage, final bounded-operation
and target preflight review, then an updated immutable package with launch enabled.
Wait for explicit Fable diagnostic-slot closure before lab work.

Acceptance: exact modules, correct selected pixels, loss-free DWM-owned DMA/fences,
all required checkpoints and no audit overflow, full live-buffer classification.
Reject missing marker, unexplained image-write map, deadline overrun or absent
baseline restoration. A passed run alone does not prove all persistent-pointer
stores covered; keep that requirement open until their writers are accounted for.
