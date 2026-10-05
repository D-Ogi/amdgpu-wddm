# M399 - Full-WDDM stop preserves disabled translation hardware

PROVENANCE: AMD amdgpu sources, MIT; local Linux v6.18 commit
7d0a66e4bb9081d75c82ec4957c50034cb0ea449. Source snapshots retained.

## Source finding

gmc_v10_0_hw_fini disables GART through gfxhub/mmhub callbacks; no boot-register
snapshot restoration follows. Each imported callback disables all16contexts,
L1 translation and L2 cache. Windows GartPrepareStop already invokes this
sequence after confirmed GFX/IH/PSP retirement. Its later GartStop nevertheless
restored pre-driver register values through WriteSnapshotBack for full WDDM
as well as display-only diagnostics. Those values include context/cache
configuration; restoring them can change the disabled final state. The exact
saved values in M398 were not captured, so a re-enabled context in that trial
and causation of its hang are NOT established.

GMC golden initialization is empty in the reviewed source; the selected
imported gfxhub_v2_0 function table has no utcl2_harvest callback. No omitted
write from either hook was identified. RLC resume order matches the reviewed
non-autoload PSP branch:stop,disableCG/PG,CSB,SPM,start. PSP resume does reload
non-PSP firmware; this is not justification to skip firmware reload.

## Change and checks

Candidate125 restores the boot snapshot only when Device->FullWddm is false.
Full-WDDM final stop retains the disabled state from successful GartPrepareStop
and releases retired ownership. FullWddm is latched during WddmInitialize and
is not cleared by WddmStop; closing the registry one-shot does not change it.
Explicit diagnostic RESTORE is unchanged. Required invalidation and existing
halt/PSP/GART-disable/storage ordering are unchanged. Unconfirmed retirement
still retains the owner; no software-success shortcut was added.

Actual-source stop chain3644checks0fail, including both successful ownership
modes and repeat-stop idempotence. Restoring unconditional snapshot replay
compiles and fails exactly2checks. Bootstrap230checks0fail. WDK and package
validation pass:25checks,0errors,0warnings,13notes. Existing compiler stack
advisories are preserved in the build log, not general stack-safety proof.

Package0.7.125.1:
SYS52A2E0FA025CCA857AE1ED918D90B26CAB64CC0CCD30DF4E4AF4101EC45EFA6E.
No deployment or runtime recovery claim. Lab remains124recovereddisplay-only,
last inspectedboot11:17:48. Next first-load GPUcontent control, then verify
newstop log/finalstate and one warm trial without a baseline OS reset.
Full M9, warm recovery, cache/lifetime scope and performance remain open.
