# M345 - Access wrappers match bare-metal MMIO; teardown phases differ

Source review after M344, not a new hardware trial. The local full Linux source
is6.18.0; E28 ran Alpine6.18.52. File hashes and line-numbered excerpts preserve
that distinction. No assumed binary identity.

1. soc15_common.h RLC_NO_KIQ uses the RLCG mediated accessor only for SR-IOV VF
   with supported RLCG access. Bare metal falls through to RREG32/WREG32.
   amdgpu_device_rreg/wreg then use readl/writel for in-BAR registers unless
   SR-IOV runtime handling applies. There is no missing bare-metal RLC enable,
   delay or wake handshake in those wrapper branches. This eliminates the
   wrapper-name hypothesis; it does not prove equivalence of all lifecycle state.
2. gmc_v10_0_flush_gpu_tlb separately chooses a firmware-mediated path when KIQ
   is ready. Its comment requires that path for GFXOFF. The direct path is for
   before KIQ/MES/GFXOFF setup. Our stopped-KIQ use must be assessed against
   retired power/translation state; the comment alone does not forbid all use
   after KIQ retirement or justify submitting work to a halted KIQ.
3. Linux ip_fini_early executes hardware fini before ip_fini releases software
   state. gmc_v10_0_hw_fini disables GFXHUB and MMHUB translation; gfxhub_v2_0
   clears16context controls and disables L1/L2 caching. The retained E28 trace
   witnesses RLC_CNTL0 at292.500195, context0 control0 at292.524654, then L1/L2
   control writes. No selected engine17invalidate-request write occurs in the
   recorded unload stream; this is not proof that no invalidation happened.
4. amdgpu_gart_unbind has a drm_dev_enter guard; PCI remove calls drm_dev_unplug
   before driver unload. Consequently the guarded unbind path can be skipped
   after unplug. This full-device Linux ownership transition cannot be copied
   as an unconditional skip into Windows GfxStop or diagnostic Fini.
5. Windows currently combines engine halt, unbind/flush and memory release in
   GfxStop, then unloads PSP and restores GART. It does not implement Linux's
   all-hardware-fini-before-software-free partition. Existing PnP comments that
   call this whole sequence amdgpu order are too strong and are corrected.

Implications: do not replace required TLB invalidation with a no-op, move an
unbind before its engine is stopped, or detach owners before a later phase can
retire their resources. A complete PnP stop needs explicit translation retirement
before release; manual diagnostic Fini must retain the live IH/GART ownership
it shares. The already imported bc250_gmc_gart_disable is a reference operation,
not yet a demonstrated warm-start remedy on this board. Linux E28 still failed
reload, so merely matching its order is not enough to claim a fix.

The source also provides gfx_v10_0_rlc_reset (two field writes with50us waits)
and a soft-reset branch that checks RLC_BUSY, stops RLC/CP/MEC, then resets.
Ordinary resume does not call that callback. If tested, it must be a separately
recorded recovery experiment with halted-engine preconditions and a successful
first-start control; neither callback existence nor busy clearance proves safe
memory release or successful subsequent PSP reload.

Next engineering work: separate PnP hardware retirement from storage destruction
without weakening the manual diagnostic path; obtain phase/ownership witnesses
before selecting a RLC recovery experiment. No unchanged warm retry or OS reset
was performed in this review. Lab remains at the last M344 observed108 display-only
state; hardware state was not re-polled in this source-only review.
