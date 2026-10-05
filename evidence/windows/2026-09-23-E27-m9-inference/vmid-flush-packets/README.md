# VMID invalidation packets and paging audit extension

Local source/host evidence only. No lab access or deployment.

BuildPagingBuffer currently handles UPDATE_PAGE_TABLE, VIRTUAL_TRANSFER and
VIRTUAL_FILL, then returns SUCCESS. FLUSH_TLB has no implementation. Microsoft
specifies the root physical address, process and affected VA range; zero/zero
means the whole range. Source checkout7515063cea4c9e98db6a92986c5b4ddb0463fd16.
Existing runtime evidence ops-gpu-016/hardware-watch.txt:100,103,112,114 records
operation12 calls, and:269 records8 calls in one summary. Repeated stream dumps
are not independent observations; do not sum them.

SubmitIbLocked already flushes each nonzero VMID before IB dispatch, even when
the root is unchanged. This may explain why ordinary inference works, but does
not by itself prove the ordering/lifetime contract of every OS FLUSH_TLB request.
VidMmUpdatePageTable performs CPU writes for GPU_PHYSICAL updates as well as
CPU_VIRTUAL. The latter is required immediately for paging-process initialization;
the correctness of the former relative to in-flight GPU reads requires review.
CPU_VIRTUAL cannot simply replace the advertised mode while directories remain
in a local segment (GPUMMUCAPS documentation).

Generalized the existing SDMA GFXHUB invalidate helper to accept VMID0..15.
VMID0 wrapper preserves the runtime GART packet stream. Selected VMID goes to
the imported AMD request encoder and selects the matching ACK bit. Invalid IDs
are refused before shifts/encoder/output. This is not an implementation of the
OS FLUSH_TLB operation yet: process/VMID binding, ordering against GFX jobs and
PTE publication must be established before calling the new entry from the DDI.
No MMHUB coverage is implied; this helper is GFXHUB only, engine0 reserved SDMA0.

run_paging.ps1 -KmdRouting -Out scratch/build/vmid-flush-test:479checks,0failures.
Includes all prior mapped-transfer routing controls,16VMIDs, undersized output,
and invalid16/UINT_MAX. The encoder is mocked; hardware ACK behavior untested.
Full KMD build/sign passes in scratch/build/vmid-flush-dev, retained0768 version,
DEVELOPMENT ONLY, DO NOT DEPLOY. Official0768 package unchanged.
Dev SYS SHA256 BAC0C7FCBC0C8F033CF5751BE79F0B152418D39DC0616155DCC77C8769401161.
