# Retained visibility diagnostics for candidate141

140 still reports hardwareblank0x101 after Windows and interactive display wake attempts. The callback breadcrumbs are absent from the preserved head/tail of the wrapping log. This instrumentation preserves the last request independently of that log; it does not change blanking behavior or force visible output.

BC250_DEVICE now retains VisibilityCalls, VisibilityLastSource, VisibilityLastRequested and VisibilityLastStatus. The normal visibility DDI publishes the last returned status for successful, invalid-source and hardware-refused requests. Its existing first32 breadcrumbs now use the per-device count. Retained fields continue updating after that limit. Zero calls distinguishes a never-issued request from the default zero/success values.

CommitPowerCalls, CommitLastPowerTransition and CommitLastPoweredOff capture every CommitVidPn entry before its power-transition early return. This helps distinguish a wake arriving only through commit/power transitions from a received visibilityTRUE. Count is all CommitVidPn calls, not only transitions. This does not implement power transitions.

WddmSummaryOf emits two short lines:
- visibility calls/source/requested/status/current SourceVisible;
- display ModeActive/DcnBlanked/DcnWriteEnabled and commit calls/power-transition/powered-off.

The normal callbacks and summary escape use existing Level Two exclusion (local MS threading-and-synchronization-second-level.md); stop summary runs after callback exclusion. No diagnostic allocation, wait, new lock, MMIO access, public ABI or version change was introduced. The quiet bugcheck restore helper is untouched.

Validation: visibility141-retained.log reports268 checks0. Actual-source visibility fixture includes40 further requests beyond the first32 logs, latest invalid source, latest hardware failure with SourceVisible unchanged, and direct actual helper checks for commit poweroff/on retention. Existing visibility/sync positive controls still pass. build141-diagnostics.log records full WDK build success, DEV SYS094929DB6BB4CCEB3E2FE8E3899CF8F1C9EDEB643336B6E444FBFC493AC7179C under scratch/build/bd013-141-diagnostics/package, metadata140, never deployed by this agent.

Changed: bc250kmd.h; display.c visibility callback plus small retained-commit helper/call; wddm.c summary only; test/display_visibility_test.c and test/generate_display_visibility_test.py. Frozen source/hash manifest/diff: retained141-source/. Parent owns candidate141 version/build/deployment and state/evidence. No lab actions.
