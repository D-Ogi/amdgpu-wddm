# Reentry capture policy, 2026-09-23

Source review found KeepLog was unconditionally replaced with zero by INF AddReg. GuardLogKeep reads this setting at each call, including the end of Bc250StopDevice. An upgrade can therefore disable an explicitly requested old-driver stop capture. This is a source-level mechanism, not proof of the precise timing of the 0799 install.

Changed only KeepLog to DWORD | NOCLOBBER (0x00010003), default zero. Explicit zero remains zero, explicit one remains one, and a first install creates zero. All hardware gates still reset on install. Guard comment updated. No deployment or device restart occurred.

Microsoft contract: https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addreg-directive (checked 2026-09-23): NOCLOBBER preserves an existing value. Standard InfVerif exits0 with pre-existing warning1199. Strict /w exits1627 with1199 both before and after this change; it concerns the existing undecorated OS target with DIRID13, not KeepLog. No clean strict-validation claim.

The additional read-only lab collection found the latest three pre-install snapshots were the successful 13:28:38 startup. It recovered no preceding stop snapshot. This does not prove a stop never ran.

CP substep dev_info records already exist in bc250_gfx_cp_resume. They are in-memory records; GfxExecute holds GartLock (fast mutex, APC_LEVEL). GuardLogKeep requires PASSIVE_LEVEL and is invoked outside GfxExecute before/after entire stages. Absence of CP substeps in the last disk snapshot cannot localize the failed instruction. Do not add file I/O inside that mutex. Further diagnosis needs finer ownership-safe persistence or a crash dump; the CP hang itself remains unresolved.
