# M661 - Exact KMD164 build

Isolated source933f383a46c761d0f4c37bbdf91c2a0cbdd017b8, parent exact1632c3849af1bc37a11986e5e820ad1fd8e7d20a0ac. Production delta: M660 BGP1 helper/call from618c2ce plus INF/ABI version164. Five pipeline test infrastructure files imported from the same commit because exact163 lacked them. Eight paths changed; main-only recovery ledger changes remain excluded.

Package version0.7.164.1, ABI0x000700A4. SYS SHA2569B9B99D3F3FA2A32816E71C8754A6BB6349A427C33CCCD1E9B09C08761849743. Both signed packages independently match every hash in the manifest; source/repository clean, deployment_source_eligible true.13 quality gates, compile/link, stack gate, catalog generation and signing pass. Largest fixed frame3992 bytes is existing GfxPagingBuildUpdate. Exact-source KMD controls pass1597 checks and ring ordering/capacity10025; one fewer ledger-specific assertion than main is expected for this base.

Build command: pwsh -File scratch/g0-kmd164-source/driver/kmd/build.ps1 -Kits P:/bc-250/toolchain/nuget -Out P:/bc-250/scratch/g0-hosted/kmd164-final001 -UmdStub P:/bc-250/scratch/g0-hosted/kmd163-final001/package-umd -QualityWorkspace P:/bc-250. The stub is unchangedD5BD1AD8, retained only as packaging input.

KeDelayExecutionThread SAL in WDK10.0.26100.0 km/wdm.h23313-23314 permits PASSIVE through APC_LEVEL. The BGP1 predicate admits no higher level. No IRQL change was needed for the bounded wait.

Not staged or deployed in this record. Exact163 remains on unit A with GPU Present/CDD interop0 and the recovered CPU desktop. Runtime validation, BGP1 content and G0 acceptance remain open.
