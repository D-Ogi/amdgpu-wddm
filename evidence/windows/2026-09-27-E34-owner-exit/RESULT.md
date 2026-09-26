# M566: imported texture survives owner-process exit

Unit A, project base a021cb1 plus the optional owner-exit control mode.
The driver artifacts are unchanged: UMD23F5269C and hosted ICD3508416F.
Control B5E59540 is built with /W4 /WX; exact hashes are in manifest.json.

CPU controls076/077 and hosted GPU controls078/079 all pass. After ten
bidirectional exchanges (20480 pixels per direction), the child creates a
68x36 shared texture and completes a GPU write. The parent opens and checks
it, then keeps its imported reference while the owner process exits. Normal
mode checks the owner's resource release and exits0. Abrupt mode terminates
the owner with42 after GPU completion, bypassing ordinary resource teardown.
The parent waits for actual process termination and validates the expected
exit code before accessing the surviving import.

Each run checks29376 pixels in the ownership phase:2448 before owner exit,
2448 after exit, and24480 across ten subsequent GPU writes/readbacks by the
survivor. There are zero mismatches. The survivor closes the import and checks
GetDeviceRemovedReason, which remains S_OK. No device-loss/Vulkan-error message
appears in either GPU run. The ordinary hosted releases have sole resource and
object references: four in078, three in079, where the terminated owner's final
DDI release is deliberately bypassed.

Global KMD live objects remain195 before/after both GPU controls. Each interval
has balanced device4/4, context8/8, process2/2 and allocation-open/close112/112
call deltas; hardware submitted/completed deltas are95/95, with timeout/refusal
counters remaining zero. These are global endpoint observations, not proof
against all transient leaks or attribution of every hardware packet.

Every runner returns0 and restores baseline UMD8279AC7F/ICD9C40083C, preserving
CPU DWM84. No window, DWM/OS restart or KMD update. Raw KMD logs remain private
with hashes; verification.json contains explicit last-summary extracts. Other
logs are preserved as captured.

This closes the tested owner-exit lifetime case, including OS cleanup after
deliberate process termination. It does not simulate a stalled GPU, unfinished
owner writes, TDR, eviction pressure or all M13.2/lifecycle requirements.
G0 and full M13 acceptance are not inferred from these controls alone.
