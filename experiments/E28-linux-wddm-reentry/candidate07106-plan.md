# M341 - RLC state around firmware reload

E28 first load enters RLC resume with enable1; reload enters with enable0. Determine whether PSP loading changes the RLC control/busy state differently on warm Windows startup. No reset hypothesis is yet accepted as a fix.

106 adds read-only RLC_CNTL and GRBM_STATUS2 snapshots before/after PSP and after GFX stop, through existing safe-read access. Persist before-PSP with the existing next GuardLogKeep, after-PSP with the next IH checkpoint. No new writes/delays or admission predicates. Build/check full package before installation.

Current105 has completed M336-M339 workloads. Preserve summary and current boot identity, STOP, clock/temp, plug availability. Install106 with all execution gates closed; this stops105, so its stop does NOT contain the new observer. Verify healthy display-only version/hash, preserve old-driver stop. One full106 start in the same Windows boot is a warm GPU attempt and obtains before/after PSP observations. Keep its original stream/handle on timeout, inspect connectivity; if truly hung use authorized plug AC recovery and recover persisted logs. Do not blindly repeat startup. If start succeeds, capture summary and then plan workload/retirement separately.

This is a changed-binary warm transition, not same-binary106 reentry acceptance. Hardware observations, not the host build, decide the next experiment. A clear busy bit is not sufficient proof of safe firmware reload or memory retirement.
