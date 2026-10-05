# M354 - Retirement TLB sequence and GFXOFF review

Read-only source review, no lab operation. Linux reference6.18.0, not exact
E28runtime6.18.52. Source hashes and numbered excerpts accompany this result.

The Windows shim retains the upstream direct GFXHUB invalidate request,
GC<10.3 dummy request read before ACK polling, bounded ACK polling, and MMHUB
semaphore protocol. Thus missing dummy-read-before-ACK is not a source defect
in this path. Windows FlushTlb also checks Sequence.Fault, preventing backend
all-ones reads after an access error from being accepted as a successful flush.
Current logs bracket the whole GFXHUB operation: they do not distinguish the
request write, dummy read, first ACK value and polling completion.

Linux chooses firmware-mediated invalidation when KIQ is ready and describes
the direct path as needed before KIQ/MES/GFXOFF setup. This is not permission
to submit to a halted KIQ at retirement. gfx_v10_0_set_powergating_state's switch
has no GC10.1.3/10.1.4 case; these reach its default no-op. A generic GFXOFF
comment therefore does not establish a missing SMU GFXOFF operation for this
APU. The RLC handshake and actual firmware power state remain separate matters.

Linux GFX hw_fini halts CP but does not itself stop RLC; generic SMU cleanup
has the conditional RLC stop. The Windows shim's declared combined fini stops
RLC itself. M333/M345 already track this lifecycle distinction; it is not a
newly discovered missing RLC stop. Both Linux gfxhub_gart_disable and the
imported Windows function disable all16contexts,L1TLB andL2cache. M348 moved
that hardware phase before storage destruction; its later GFXHUBflush still
produced the busy transition. Ordering alone did not fix warm reentry.

amdgpu_gart_unbind checks drm_dev_enter and updates invalid PTEs before
amdgpu_gart_invalidate_tlb. Linux PCI unplug can bypass this path (M345);
Windows cannot copy that behavior without proving its own retirement contract.
Keep required invalidations and page-retention decisions. Linux's explicit
mb/HDP contract and Windows register/mapping ordering are not newly certified
by this comparison; cache/PFN ownership acceptance stays open.

Next experiment: add scoped observation of the existing request write,
dummy request read and final ACK sample/result during retirement only. Retain
original MMIO access order and every existing poll/semaphore operation; no
extra semaphore read. Record ACK value and return status alongside RLC state.
Host validation must show normal flush behavior unchanged and distinguish
successful/stuck ACK paths. Hardware measurement then resolves which substep
first accompanies RLCbusy; it must not equate ACK with complete RLC retirement.
No new reset mask, GFXOFF mailbox command or TLB-skip policy follows from this
review. Latest lab remains111display-only as M353; warm startup is unresolved.
