# M358 - Owner-scoped GTT mapping retirement, local only

GpuMemRetireGttMappings invalidates active-sequence GTT PTEranges, performs one
existing both-hub FlushTlb, and only after success sets TranslationsRetired.
Used,Owner,Cpu,Mc,Bound,Retired and backing storage remain unchanged. Bound is
still the historical exposure flag: successful unbind cannot bypass GpuQuiet
in GpuMemRelease. The new flag prevents a later mem_free from invalidating an
already retired mapping again. VRAM and other owners are untouched.

TlbDirty is set before unbind. Unbind or flush failure returns without committing
any new per-entry retirement flags. There is no allocation release in this
operation. An idempotent call with no work performs no extra flush; an existing
dirty state still reports failure. The caller contract requires GartLock and
prior drain/halt of this owner's CP/SDMA consumers. The function does not itself
prove that contract or establish RLCquiet.

Actual-source extraction compiles the entry/pool structures, MemOf,
GpuMemRetireGttMappings,bc250_shim_mem_free andGpuMemRelease.38checks pass across
success,second-unbind failure,flush failure,plan refusal and sequence-fault
refusal. The positive case verifies exact two ranges/pagecounts,foreignowner/
VRAM unchanged,backing retention through flush,idempotence,no repeated flush
at mem_free,and release only after GpuQuiet. Mocks cover unbind/flush/free;
these checks do not prove actual PTEencoding or hardware behavior.
Initial harness compilation needed removal of redundant SALdefines and an
empty Cstruct; production code was not changed for those harness issues.

WDKbuild succeeds. Local build retains112metadata and has SYS
424B0CA9A5268811EDA61DBC5F52D5CAA259D643C07D910C1C82BA31352C19F8;
it is an undeployed intermediate artifact, not installed candidate112 fromM356.
No KMD stop/start path calls the new helper yet. Current hardware remainsM356.

Next integrate after CP/SDMAhalt with RLC/CSB and firmware retained, then stop
RLC before PSP/GART retirement and final storage destruction. Require successful
owner-specific unbind/flush before that generation can be accepted as quiet.
Preserve diagnosticFini and partial-start ownership. Hardware positive control,
retirement witnesses and warm reentry remain required. General DMA/cache/alias/
resource readiness acceptance is not completed by this local extraction.
