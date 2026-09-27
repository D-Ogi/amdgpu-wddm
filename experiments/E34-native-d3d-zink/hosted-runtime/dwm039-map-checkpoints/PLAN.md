# DWM039 checkpoint trial - prepared locally, not run

Hypothesis: M687's measured copy detector can bracket DWM rendering and isolate
primary/GDI capture reads on UMDCC82A2D9, preserving correct composition and GPU work.

Prepared from DWM037 at3ca3217; new directory and tasks039. Candidate Mesa0185cb8d,
hosted ICD3508416F, baseline KMD164/UMD8279/ICDCF39. Enable uploader and map markers
only in the selected DWM process. Each capture has separately acknowledged bounds.
The CPU audit checkpoint is not a GPU fence.

One clock starts before DLL/gate mutation. Planned render stop105s, final marker
limit130s, independent rollback140s, measured total acceptance<=180s. OS stalls
can exceed deadlines; a watchdog does not prove a hard real-time guarantee.

Local validation completed: router/control build with /W4 /WX,23 scripts parsed
under Windows PowerShell5.1,7 watchdog decision controls, and durable copy/restore
controls (including corrupt-backup fallback and preservation of unknown files).
Worker failures clear success; watchdog requires restoration evidence, including
when the worker already wrote done.json. Trial deadlines use shared QPC ticks.
Critical durable receipts publish by same-directory rename after Flush(true).
These are host controls, not a measured target watchdog/recovery exercise.

The initial draft package remains archived locally. The reviewed package003 has25
hashed files, manifestEB24251FE95EE5FC97FE37C3FEF20EC1BDC55E9A225BA3C23FC11982AD0B1FFB.
Router465F0219/control6B188424 are pinned by stage.py alongside the two DLLs;
compiled-source hashes must match the retained build receipt. Package is local,
not deployed. Launch verifies the exact staging path and every manifest entry
before creating a task; the worker checks watchdog PID/start before mutation.

Five host interruption cases restore both fake baseline files after0..4 DLL move/
copy steps, using the real durable restoration helper. This covers file mutation
boundaries only, not hardware or OS scheduler faults. The installation and restore
share the same named mutex; the watchdog publishes abort before stopping the worker
and restoring. The worker checks abort while holding that mutex.

The worker saves a fresh read-only preflight before any mutation, checking the
CPU DWM, exact baseline binaries, loaded ABI, confirmed health, clock, temperature,
STOP, competing work and latched gates. Diagnostic slot closure is recorded in
local coordination message151; the subsequent reboot requires a new baseline.

Next operational steps: read fresh
lab baseline/STOP/temperature, stage this exact package, run verify-stage.ps1, announce
through the overlay, launch once, observe the same task to terminal, then collect
and independently validate images/ETW/audit/rollback. Never rerun on observation
timeout. Preserve a failed run and its original tasks until terminal state is known.
Target watchdog recovery and overall180-second elapsed time are unmeasured until
that run. G0 acceptance remains open.

Acceptance: exact modules, correct selected pixels, loss-free DWM-owned DMA/fences,
all required checkpoints and no audit overflow, full live-buffer classification.
Reject missing marker, unexplained image-write map, deadline overrun or absent
baseline restoration. A passed run alone does not prove all persistent-pointer
stores covered; keep that requirement open until their writers are accounted for.
