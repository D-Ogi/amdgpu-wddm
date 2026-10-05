# BD-013/014 source work; BD-016/019 triage

Not deployed. Frozen138 is unchanged. Kernel observation instrumentation will follow this snapshot separately.

BD-013: full-WDDM visibility now uses inherited OTG0 blanking through AMD optc1_set_blank behavior, preserving surface pixels and scan timing. It changes BLANK_DATA_EN/BLANK_DE_MODE and disables blank-data double buffering on hide; other fields and inherited black color remain unchanged. It confirms CURRENT_BLANK_STATE with a bounded poll. SourceVisible changes only after success. Hide no longer disarms requested vsyncs. Quiet DcnSetVisibility(TRUE) is integrated in POST/bugcheck restore, including hidden-before-first-flip. It takes no lock, makes no graphics callback, allocates/logs nothing, and uses the existing 100us/50ms explicit stall bound. GPU cancellation/reset at bugcheck remains outside this fix.

Scope: legacy display-only mode without VidPnFlip/MMIO permission still has CPU-clear fallback. No gates were broadly enabled. FIXED may describe only the full hardware visibility path; this is not all-profile non-destructive blanking acceptance.

BD-014: DcnVsyncEnable uses DxgkCbSynchronizeExecution (message0) for register RMW and DcnVsyncArmed publication under the ISR lock. It is shared by WddmVSyncArm, stop and gfx rearm. WDDM lock orders caller decisions; the synchronized callback never takes it. Writes are quiet. ISR and enable clear only VUPDATE_NO_LOCK; other W1C command bits are masked from readback before writing. Failed synchronization does not publish armed state.

BD-016: TRIAGED, no write added. Existing scope is inherited OTG0 only, where selector0 agrees with Linux optc1_lock(inst=0), factsM87 and firmware analysis. A future own-mode/ODM/multiple-OTG path must program the selector explicitly; this is not closed globally.

BD-019: TRIAGED, no blind disable. M86 records HUBP1..3 in blank and assigned inactive VTG1..3, OTG1 disabled; firmware leaves clocks running. The GOP report explicitly says leave these until own modeset. Linux disable_plane/init_hw is a complete resource/modeset transition, not a reason to disable arbitrary inherited pipes. Power and future own-mode cleanup remain open, not a current demonstrated scanout fault.

Tests:
- visibility-arm.log:109 checks0, actual synchronized enable/ISR, WddmVSyncArm, WddmSourceVisibility, DCN blank helper and source-visibility DDI. Repeated hide/show preserves pixels, unrelated fields and vsync; fake IRQ lock checks surround MMIO and armed publication.
- post-restore-hidden.log:573 checks0, actual restore and bugcheck callbacks; explicitly includes hidden-after-flip and hidden-before-first-flip, alongside prior restore coverage.
- Mutations: bypass synchronization19 failures; acknowledge unrelated W1C1; clear primary instead of hardware blank22; hide stops vsync10; omit restore unblank1. This is a deterministic lock/MMIO model, not hardware or multicore stress.
- Full WDK build passes, build.log. DEV SYS FAE97F8A61F46AC4ED12A2F9D8A99563DBC5D66923731789810BBAB3FA8FF5F7 in scratch/build/bd013-014/package; unchanged shared138 metadata, never deployed. Combined working tree may include peer timing code. Tests after build changed only fixtures.

References (local):
- ddi-display/dispmprt.md:2815 onward: graphics synchronization callback is <=DISPATCH_LEVEL, locks against InterruptRoutine, message number0 for this single-message adapter (M38).
- ddi-display/d3dkmddi.md:13013 onward: visibility DDI controls scanning and reports status; :35739 explicitly retains vsync with visibilityFALSE.
- linux-src @7d0a66e4bb9081d75c82ec4957c50034cb0ea449, AMD display MIT: optc/dcn201/dcn201_optc.c dcn201_tg_funcs:set_blank -> optc1_set_blank; optc/dcn10/dcn10_optc.c:390-477 blank/double-buffer, :673 lock selector.
- factsM86/M87 and linked immutable E21 dumps/trace; firmware/bios/analysis/gop-dcn/GOP-DCN.md around355-389 for inherited inactive pipes and D4 scope.
- Timing peer added exact generated names/read permissions/write permissions for mmOTG0_OTG_BLANK_CONTROL and mmOTG0_OTG_DOUBLE_BUFFER_CONTROL. Other timing additions are read-only. Public75-register escape remains unchanged; no literal MMIO offset was introduced.

Suggested comments/statuses: BD013 FIXED (full-WDDM source only, legacy fallback explicit); BD014 FIXED (source, hardware acceptance pending); BD016/019 TRIAGED as future own-modeset/ownership work. No shared backlog changed here.

Hardware acceptance still needed: positive raw read of blank/double-buffer fields and inherited black color before first new write, bounded hide/show while preserving content and requested vsync, then normal restore/handover. No lab action occurred in this task. Main owns evidence/status and final protocol/integration build.
