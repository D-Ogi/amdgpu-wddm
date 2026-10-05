# Logical table SAVE/RESTORE foundation

Refactor the existing byte-copy loop into CopySlotBytes and add PagingPtShadowSaveBytes/RestoreBytes. Scratch is a separate caller-owned slot, never registered in the table map. SAVE resets its Known bitmap before copying source bytes/knowledge to offset0. RESTORE copies knowledge and values to the destination band. Missing/untracked source bytes remain unknown; existing in-place overlap direction is preserved. No allocation, hardware access or retained pointer. The caller must reserve scratch outside the kernel stack and serialize a complete group under the table-shadow lock.

Host2434928checks PASS; kernel-flag compilation PASS. New124three-page cycle fixtures cover62offsets in steps of67, each with full remaining band and up to17bytes. Each page has distinct values and unknown-byte positions. An independent original-slot snapshot verifies knowledge for every byte and values where known, including bytes outside the copy band. New SAVE of unknown data clears previous scratch knowledge. Existing shadow tests also pass. Actual KMD routing16348checks PASS after the copy-loop refactor.

Generated mutation replaces restore's scratch source with unknown source;2434928checks/118403failures, native exit1. Printed failures are capped at10 in the generated test copy to bound log size; all checks execute. Production source is unchanged by the mutation.

Not integrated into virtual/local graph publication yet. Captured physical plans must survive the appropriate OS multipass lifetime when copies modify their own translation tables; re-resolving modified VAs on resume would change the operation. Microsoft local d3dkmddi.md4132/4144 documents opaque progress preservation, not ownership of arbitrary retained pointers. Scope must include successful retirement, cancellation and device stop. System-only M291 captures do not write local table storage; general table/mixed graph acceptance remains open.

No lab mutation or WDK package deployment. Both configured TCP22routes failed again this turn; owner sshd status is pending.
