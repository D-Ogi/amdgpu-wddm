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

Remaining before staging: fault-injection review of the shared installation/restore mutex and abort handling,
watchdog closure/error receipts and cleanup integration, bounded startup/final
operations, package generation and exact candidate pins, rebuilt router/control identity,
PowerShell parsing and fault controls. No manifest has been generated. Do not run
this draft. Wait for explicit Fable diagnostic-slot closure before lab work.

Acceptance: exact modules, correct selected pixels, loss-free DWM-owned DMA/fences,
all required checkpoints and no audit overflow, full live-buffer classification.
Reject missing marker, unexplained image-write map, deadline overrun or absent
baseline restoration. A passed run alone does not prove all persistent-pointer
stores covered; keep that requirement open until their writers are accounted for.
