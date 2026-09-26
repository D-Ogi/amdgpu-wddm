# M266: explicit cache intent across BC2A v2 (local only)

2026-09-23. Base bed764da5192be7132d646be0e6331c1edeb30fd, uncommitted sources captured. No lab mutation or deployment.

M265's observed gap spans both ends: Windows winsys zero-initialized gem_flags without assigning it; the KMD allocation reader also omitted it. Changing only KMD would incorrectly interpret legacy command allocations as cached. BC2A v2 retains the192-byte layout but declares gem_flags authoritative. Existing BC250_UMD_ALLOC_VERSION remains1 for legacy producers; new VERSION_CACHE_POLICY is2. New winsys emits2 and maps RADEON CPU_ACCESS/NO_CPU_ACCESS/GTT_WC to GEM bits0/1/2, matching local radv_amdgpu_bo.c and the UAPI.

KMD retains the full64-bit field, marks cache intent valid only for v2+, and requests Cached for GTT without NO_CPU_ACCESS or USWC. Version1, VRAM and USWC retain Cached0. GPU MMU now advertises CacheCoherentMemorySupported; the existing PTE encoder already maps OS CacheCoherent into AMDGPU_PTE_SNOOPED. Allocation diagnostics include gem flags and selected Cached. No claim that this closes borrowed CPU_VIRTUAL table aliases or verifies OS PTE choices at runtime.

Local Microsoft references: enriched ddi-display/d3dkmddi.md18223 describes Cached and default WC backing store;22240 describes CacheCoherentMemorySupported. Original docs commit7515063cea4c9e98db6a92986c5b4ddb0463fd16. Local Mesa radv_amdgpu_bo.c around365-374 maps cache/access intent to GEM flags. Incremental MIT Mesa patch applies after the consolidated BC250 patch; reverse-check passes against the built tree.

Validation:4096 heap/flag/alignment combinations check full field preservation and v2 policy, plus v1 compatibility for every combination; existing contract/parser controls and kernel-mode compile-check pass. PTE regression suite passes including CacheCoherent combinations and2048 dedicated-table cases. Signed WDK DEV build and ICD rebuild pass. These are source/build results, not live cache-coherency acceptance.

DEV KMD SHA256 A784722B20B7BD2C76DB21774622233929D6986B9031D208BBAD742080E3441F. ICD SHA2566B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754. KMD still uses existing development version and must be versioned before installation. Pair not deployed; installed0798/quiet ICD unchanged. Next validate paired runtime cached allocations, actual coherent PTEs and regression controls before performance claims. M259-M262 local changes are included in this DEV KMD.
