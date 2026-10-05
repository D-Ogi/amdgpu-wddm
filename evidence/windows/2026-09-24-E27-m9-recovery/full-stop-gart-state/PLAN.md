# M399 - Preserve disabled translation state at full-WDDM stop

Source review: Linux v6.18 gmc_v10_0_hw_fini calls gart_disable and does not
restore boot register snapshots. Imported gfxhub/mmhub disable all contexts,
L1 translation and L2 cache. Windows GartPrepareStop does this, but GartStop
later calls WriteSnapshotBack for every enabled owner, restoring pre-driver
values including context/cache configuration. Diagnostic restoration policy
therefore also affects full WDDM, after its owned tables have been modified.

Change final full-WDDM stop to retain the disabled state established by the
existing successful hardware-retirement phase. Keep snapshot restoration for
display-only diagnostic ownership and the explicit diagnostic RESTORE command.
Do not change halt, invalidation, PSP, GART-disable or memory-lifetime ordering.
Host positive paths must cover both modes, release and repeat-stop idempotence;
restoring the unconditional snapshot call is the negative control. Build/package
before hardware first-start, stop and one warm trial. No causal claim yet.
