# Isolated KMD161 with retained observation times - M647

Final source: 6c93d74e4876b66e04a39973a2c1e1ebdb9ec418.
Parent exact160/a1f3bf9; imports b9a0276 and ba3f4c2 only for wddm.c,
plus INF/escape version identifiers. No unrelated main-branch fence changes.
Final SYS SHA256: 40F7916FE24957666F392D5913EE70C7DCFEB1A23A121C39E6DB0ACE907B6FF1.
Package-umd version0.7.161.1; escape0x000700A1.
Local artifacts: scratch/g0-hosted/kmd161-final002.

All13 quick gates, full compile/link, stack budget, catalog creation and signing
commands pass. Largest fixed stack3992 bytes is the existing GfxPagingBuildUpdate
warning. Final source/artifact manifest is clean and eligible; every packaged
file was independently rehashed against it, and both packages carry the same SYS.
Target load/signature acceptance is not established by this host build.

The retained observations and successful returned-cap counters are diagnostic.
Timestamps distinguish startup Blts from sustained desktop work; QPC permits
ETW correlation, interrupt time provides per-boot chronology. Neither a locked
metadata snapshot nor a backing-store lock event proves pixel copying, residency
through retirement, or synchronization between rendering and Present.

An earlier final001 package8B7C289C from85667a7 lacks observation timestamps and
was never staged/deployed. Keep it distinct from final002/40F7916F. During time
patch preparation an overbroad whole-file equality assertion stopped before
build: the isolated baseline intentionally excludes unrelated main fence ledger
changes. Exact reverse-patch verification and equality of the observation type,
observation function and DriverCaps function confirmed the intended import.
No source changes beyond the imported patch were needed to resolve this check.

Build command:
pwsh -NoProfile -File scratch/g0-kmd161-source/driver/kmd/build.ps1
-Kits P:/bc-250/toolchain/nuget -Out P:/bc-250/scratch/g0-hosted/kmd161-final002
-UmdStub P:/bc-250/scratch/g0-hosted/kmd157-final001/package-umd
-QualityWorkspace P:/bc-250

No lab mutation, staging or trial in this record. KMD160 remains the recorded
lab version; exact160 is the required rollback. Before another interop trial,
resolve the existing ETW attribution by allocation lifetime and adapter epoch.
A repeated quiet desktop alone cannot establish the normal G0 Present path.
