# CP startup checkpoints (local), 2026-09-23

M267 stopped after the persisted stage6 entry. Existing substep logs were only in memory. The shared shim now exposes eight ordered CP steps: KIQ initialization, compute initialization/unhalt, KCQ enable, graphics queue initialization, KGQ enable, graphics start, graphics ring tests, compute ring tests. The ordinary CP entry point executes those same steps in order; no MMIO sequence is duplicated.

Only traced unpublished KMD startup uses the individual steps. It writes before/after snapshots outside GfxExecute, after GartLock/GfxPagingLock release and at PASSIVE_LEVEL. Internal CpStepDone enforces order; ordinary RUN cannot skip a partial CP sequence. StagesDone still records hardware touched for unwind, not readiness; access opens only at stage8. Escape wire ABI is unchanged. KeepLog=0 uses the existing one-call initialization path.

Validation:
- run_gfx.ps1: exact Linux replay354+35writes with24address exceptions; eleven ring tests and rerun/unclean/stuck/SDMA controls pass. Four intentional controls fail as expected. User and kernel shim compilation pass.
- generate_cp_startup_test.py extracts actual GfxInitializeHardware and CP enum. Nineteen host scenarios cover complete traced/untraced paths and stopping at each CP failure (NTSTATUS and shim result). Zero failures under MSVC /W4 /WX /O2 /std:c11. GfxExecute and logging are mocked: this proves coordinator order, not actual locking, file durability or hardware completion.
- Signed WDK DEV build passes. SYS SHA256733035A59DE47646D138224FCA61283FBFC2F18C9BD990E283D28CD889640136, scratch/build/cp-steps-dev. Version remains0799; NOT a deployment package until version is bumped and packagecheck is run. Final source differs from build only in comments/indentation.

No lab mutation, new GPU test or reboot. Installed0799 and old quiet ICD remain unchanged. CP root cause, runtime checkpoint durability, startup/reentry acceptance, cache policy and the complete M9 objective remain open.
