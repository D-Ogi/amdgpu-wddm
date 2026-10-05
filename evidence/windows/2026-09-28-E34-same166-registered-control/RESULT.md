# M714: same166 registered-node install passes; verification job times out

Unit A, runner2cff2e6, manifest438CEDDDB43B45D6C0A9576BE7AA6EBCE3590EF83D7682658633C7BAB0014EFB. Same166 package in both labels, selector005 from dde99a4. Task starts03:29:18Z, supervised work reports115.106 seconds, recovery-required/restore-unverified. This is not an accepted end-to-end transition.

The registered published INF supplies exactly one version0.7.166.1 node. DIF_INSTALLDEVICE returns success and installed_deferred=true; problem22 remains, restart bits absent under the helper predicate. Install phase completes, and Rebind reuses that completed disabled binding. Configure and Enable complete. This validates same-image installation/start suppression and retained binding on this run. It does not validate166-to169 image replacement or establish the sole cause of M712 E0000217.

Verify reaches two matching CPU DWM/health samples (DWM2652, generation27099748258/epoch5, flags7, fresh completed work), then its70-second job times out. No readiness summary or later health-wait receipt exists. Job closure is confirmed. Source inspection identifies potentially expensive serialization of Get-ItemProperty provider metadata in the readiness summary; this is a hypothesis about the stall location, not a sampled process stack.

The supervisor restores the exact prior SetupAPI LogLevel536887297 and ends. Independent final preflight03:32:18Z passes: exact166/AA77, CPU UMD8279, baseline ICDCF39, health15, same generation/epoch,67.2C. Windows boot and CPU DWM2652 are retained. No reboot, manual confirmation or power cycle. Actor of later health confirmation is not established. Terminal task removed after this independent check; lab free.

Selected receipts are unmodified; final-cpu-selected.json is a field selection from local preflight, excluding device identifiers and registry/log contents. Full receipts remain scratch/g0-hosted/kmd169-control166-001-ops/receipts.tar. No raw memory or secrets are included. The failed automatic closure remains distinct from independently verified CPU recovery. G0 remains open.
