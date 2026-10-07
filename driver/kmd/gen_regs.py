#!/usr/bin/env python3
"""Generate regs.generated.h for bc250kmd: every BAR5 offset the miniport may touch, by name, from regcalc.

Nobody types an offset (docs/02-register-addressing.md). Two tables come out of this script:

  g_MmioReadAllow   the offsets the escape READ_REG may read: tools/win/bc250rd plus named positive controls, i.e.
                    registers that were read on unit A without harm and without side effects (facts M16, M25)
  g_MmioWriteAllow  the offsets the escape WRITE_REG may write. A register gets onto WRITABLE below by an
                    experiment that says why, never by convenience.
  g_MmioGartAllow   the offsets the kernel's GART command (gart.c) may touch, and nobody else: exactly the
                    registers amdgpu itself wrote on unit A during that step of its init, taken from the
                    recorded trace (E03), plus the two acknowledge registers it polled. Not chosen by us.
  g_MmioDcnAllow    ADR 0011 point 3: the offsets the read-only DCN dump (dcn.c) may read. Not from a trace -
                    there is none yet for this IP under Windows, which is what this table is the first step of -
                    but from DCN_REGISTERS below, computed the same way as every other table here.
  g_MmioDcnWriteAllow  ADR 0011 point 3 / 0.7.20: the six HUBP0/OTG0 registers the gated flip (dcn.c's DcnFlip)
                    may write, from DCN_WRITE_REGISTERS below - the M87 flip sequence, minus the two registers
                    (FLIP_CONTROL2, VUPDATE_KEEPOUT) and the OTG0_OTG_GLOBAL_CONTROL0 write amdgpu issues but
                    that were already at the value it wrote (facts M87 note, ADR 0011 step 2 of E22). 0.7.24
                    (ADR 0011 point 3 step 3) adds OTG0_OTG_GLOBAL_SYNC_STATUS: the VUPDATE_NO_LOCK_INT_EN /
                    _EVENT_CLEAR read-modify-write the hardware vsync interrupt is enabled and acknowledged
                    with (facts M88), the same register DcnEscape already reads by name.

Run:  python driver/kmd/gen_regs.py
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "regcalc"))
sys.path.insert(0, str(ROOT / "tools" / "diagusb"))
sys.path.insert(0, str(ROOT / "tools" / "trace"))
from regcalc import HDR_DIR, RegMap  # noqa: E402
from gen_probes import SPEC  # noqa: E402  (ip -> header)
from extract_phase import accesses  # noqa: E402

TRACE = ROOT / "evidence/linux/2026-09-21-E03-init-trace/amdgpu-events.txt"

# gfx.c's stage C submission (ADR 0008) programs one VMID's page directory root through
# gfxhub_v2_0_setup_vm_pt_regs(), and no trace of ours holds those registers: amdgpu writes them per
# job from amdgpu_vm_flush(), while every window we recorded is a bring-up. VMIDs 1..15, both halves
# each, by name - upstream reaches them as mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 plus
# hub->ctx_addr_distance * vmid, and that distance is defined as the CONTEXT1 offset minus the
# CONTEXT0 one (gfxhub_v2_0.c:459-460), so naming the contexts is the same addresses by the other
# road. VMID 0 is the GART's own and bc250_gmc_set_vmid_pd() refuses it, so it is not here. The
# invalidation that goes with a root change is GC.GCVM_INVALIDATE_ENG17_REQ/ACK, which the Gfx table
# already holds: gpumem.c flushes the TLB after every bind.
VMID_PAGE_TABLE_BASE = [
    ("GC", f"mmGCVM_CONTEXT{vmid}_PAGE_TABLE_BASE_ADDR_{half}",
     f"not traced: the page directory root of VMID {vmid}, ADR 0008 stage C")
    for vmid in range(1, 16) for half in ("LO32", "HI32")
]

# (table, experiment, regex on traced register names, the step's time in seconds since the first access: its end,
#  or a list of (since, until) windows in which case the registers amdgpu only read there count as well,
#  registers the step only reads). gmc_v10_0_gart_enable() is over at 0.26 s (docs/init-sequence.md).
SEQUENCES = [
    ("Gart", "E09", r"^(GC\.(GCVM|GCMC)|MMHUB\.(MMVM|MMMC))", 0.26,
     [("GC", "mmGCVM_INVALIDATE_ENG17_ACK"), ("MMHUB", "mmMMVM_INVALIDATE_ENG17_ACK")]),
    # psp_v11_0_8 ring create and the eleven submissions are over at 0.309 s; the SMU mailbox follows.
    ("Psp", "E10", r"^MP0\.MP0_SMN_C2PMSG_\d+$", 0.309, []),
    # M5 second part. 0.038 s: nv_common_hw_init() opens the doorbell aperture (closed under Windows, E02).
    # 0.5496 to 0.551 s: gfx_v10_0_hw_init() and sdma_v5_0_hw_init() (golden registers, constants, RLC, KIQ, MEC,
    # the queues, the ring tests, both SDMA rings). 1.560 to 1.562 s: interrupt state (amdgpu_fence_driver_hw_init,
    # gfx_v10_0_late_init, amdkfd's pipe interrupts) and the doorbell self-ring aperture.
    # GCMC_VM_CACHEABLE_DRAM_ADDRESS_END: one of AMD's golden settings for this part (driver/shim/generated/
    # gfx10_golden_cyan_skillfish.inc), the only GCVM/GCMC write of the step (0.550 s, 0x000FFFFF).
    # 0.2495 to 0.2528 s: nothing but the TLB flush of both hubs after each bind of a ring into the GART
    # (amdgpu_gart_invalidate_tlb), which gpumem.c does after its own binds.
    ("Gfx", "E11", r"^(GC\.(?!GCVM_|GCMC_)|GC\.GCVM_INVALIDATE_ENG17_(REQ|ACK)$|GC\.GCMC_VM_CACHEABLE_DRAM_ADDRESS_END$|MMHUB\.MMVM_INVALIDATE_ENG17_(REQ|ACK|SEM)$|"
                   r"NBIO\.(RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN|BIF_SDMA[01]_DOORBELL_RANGE|"
                   r"BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_(BASE_LOW|BASE_HIGH|CNTL))$)",
     [(0.0375, 0.0385), (0.2495, 0.2528), (0.5496, 0.551), (1.560, 1.562)],
     # The one register of this table that no trace of ours holds: the undo asks the MEC to let go of the KIQ's queue
     # before it halts the engine (bc250_kiq_dequeue(), upstream's handshake of kgd_hqd_destroy(),
     # amdgpu_amdkfd_gfx_v10.c:606-619). Without it the MEC keeps the queue's fetch state across the halt and the next
     # bring-up faults at the old ring's address (facts M44, E12 run 002).
     [("GC", "mmCP_HQD_DEQUEUE_REQUEST", "not traced: the undo's dequeue handshake, facts M44"),
      # M15.12 stage 1: the wave kill of amdgpu gfx_v10_0_ring_soft_recovery (SQ_CMD CMD=KILL, MODE=BROADCAST,
      # CHECK_VMID, VM_ID). Writable through the GFX table so the shim's bc250_gfx_soft_recover_vmid can issue it;
      # reached only from DxgkDdiResetEngine under the HangRecoveryMode switch. Already on the escape read list.
      ("GC", "mmSQ_CMD", "not traced: M15.12 soft-recovery wave kill, amdgpu gfx_v10_0_ring_soft_recovery"),
      ("GC", "mmGRBM_STATUS2", "read observation for opt-in RLC reload reset, M349"),
      ("GC", "mmGRBM_SOFT_RESET", "not traced: AMD RLC reset callback, opt-in M349"),
      ("GC", "mmRLC_SAFE_MODE", "not traced: AMD paired safe-mode scope, M370"),
      ("GC", "mmSDMA0_FREEZE", "not traced: AMD queue quiescence, M370"),
      ("GC", "mmSDMA1_FREEZE", "not traced: AMD queue quiescence, M370"),
      ("GC", "mmSDMA0_STATUS1_REG", "not traced: AMD idle fallback, M370"),
      ("GC", "mmSDMA1_STATUS1_REG", "not traced: AMD idle fallback, M370"),
      # The CU mode (docs/design/cu-mode.md, driver/shim/bc250_cu_mode.c): amdgpu never touches the SPI's
      # dispatch gate, the bc250-40cu-unlock reference writes it next to CC_GC_SHADER_ARRAY_CONFIG (already
      # here from the trace). Written only when CuMode is 40, or to put back this boot's stock value. The RLC's
      # always-on WGP mask is read, never written; RLC_PG_CNTL (traced) is read as the precondition of 40.
      ("GC", "mmSPI_PG_ENABLE_STATIC_WGP_MASK", "not traced: CU mode dispatch gate, docs/design/cu-mode.md"),
      ("GC", "mmRLC_PG_ALWAYS_ON_WGP_MASK", "not traced: CU mode observation, read only")]
     + VMID_PAGE_TABLE_BASE),
    # M6: navi10_ih_irq_init() on unit A, 0.252832 to 0.252845 s: the IH ring's registers, the dummy read address and
    # the bus master bit of the interrupt controller, the IH doorbell range. 19 accesses, nothing else in the window.
    # The ring is GTT memory: gpumem.c flushes the TLB after the bind as amdgpu did (its binds of 0.2495 to 0.2528 s,
    # the IH ring's among them), so the five registers of that flush belong to this sequence as well.
    ("Ih", "E12", r"^(OSSSYS\.IH_|NBIO\.(INTERRUPT_CNTL2?|BIF_IH_DOORBELL_RANGE)$|GC\.GCVM_INVALIDATE_ENG17_(REQ|ACK)$|"
                  r"MMHUB\.MMVM_INVALIDATE_ENG17_(REQ|ACK|SEM)$)", [(0.2495, 0.25290)],
     # Not traced: amdgpu's own bring-up (navi10_ih_irq_init) never reads this status register, so E12's window
     # never captured it. Added read-only for docs/design/vsync-interrupt-route.md's diagnostic dump: IDLE,
     # INPUT_IDLE and BIF_INTERRUPT_LINE are IH's own view of whether anything is incoming, distinct from the
     # ring's rptr/wptr (which only reflect an entry already written into the ring).
     [("OSSSYS", "mmIH_STATUS", "not traced: read-only diagnostic, docs/design/vsync-interrupt-route.md")]),
]

# (ip, register, experiment that put it here, why it is safe)
WRITABLE = [
    ("GC", "mmSCRATCH_REG0", "E07", "scratch, no function; amdgpu's own ring-test target, written 11 times during init (facts M24)"),
    ("GC", "mmSCRATCH_REG1", "E07", "scratch, no function; second control so that one lucky address cannot pass the test"),
]

# Named offsets the driver's own code uses (beyond the tables).
NAMED = [("NBIO", "mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE"), ("GC", "mmSCRATCH_REG0"), ("GC", "mmSCRATCH_REG1"), ("GC", "mmGRBM_STATUS"),
         ("GC", "mmGCMC_VM_FB_OFFSET"), ("GC", "mmGCMC_VM_FB_LOCATION_BASE"), ("GC", "mmGCMC_VM_FB_LOCATION_TOP"),
         # gfx.c: read-only RLC retirement observation, no new write permission
         ("GC", "mmGRBM_STATUS2"), ("GC", "mmRLC_CNTL"),
         # dpm.c: the busy sampler's second engine, the paging node's SDMA0 (read only)
         ("GC", "mmSDMA0_STATUS_REG"),
         # gfx.c: are the engines halted?
         ("GC", "mmCP_ME_CNTL"), ("GC", "mmCP_MEC_CNTL"), ("GC", "mmSDMA0_F32_CNTL"), ("GC", "mmSDMA1_F32_CNTL"),
         # gfx.c: a stage that stopped half way must not leave a me/pipe/queue selected
         ("GC", "mmGRBM_GFX_CNTL"),
         # test/hang_recovery_test.c: the M15.12 wave kill's register, to check which sequence table holds it
         ("GC", "mmSQ_CMD"),
         # gfx.c: the one write that passes a stopped sequence (gpumem.c flushes the TLB after its binds)
         ("MMHUB", "mmMMVM_INVALIDATE_ENG17_SEM"),
         # gfx.c: a PLAN answers the GRBM CAM probe, which writes one of these and reads the other
         ("GC", "mmVGT_ESGS_RING_SIZE"), ("GC", "mmVGT_ESGS_RING_SIZE_UMD"),
         # KMD193, ih.c: the UTCL2 fault latch the gfxhub keeps for the first fault of a burst, read once per
         # burst in the IH DPC (read only; amdgpu reads the same three in gfxhub_v2_0_print_l2_protection_fault_status).
         ("GC", "mmGCVM_L2_PROTECTION_FAULT_STATUS"), ("GC", "mmGCVM_L2_PROTECTION_FAULT_ADDR_LO32"),
         ("GC", "mmGCVM_L2_PROTECTION_FAULT_ADDR_HI32"),
         # KMD193, wddm.c: the CP/GRBM snapshot taken once per HARDWARE FENCE TIMEOUT (read only). GRBM_STATUS,
         # GRBM_STATUS2 and CP_ME_CNTL are already named above for gfx.c.
         ("GC", "mmCP_RB0_RPTR"), ("GC", "mmCP_RB0_WPTR"),
         ("GC", "mmCP_IB1_BASE_LO"), ("GC", "mmCP_IB1_BASE_HI"), ("GC", "mmCP_IB1_BUFSZ"),
         ("GC", "mmCP_IB2_BASE_LO"), ("GC", "mmCP_IB2_BASE_HI"), ("GC", "mmCP_IB2_BUFSZ"),
         ("GC", "mmCP_STAT"), ("GC", "mmCP_BUSY_STAT"),
         ("GC", "mmCP_STALLED_STAT1"), ("GC", "mmCP_STALLED_STAT2"), ("GC", "mmCP_STALLED_STAT3"),
         ("GC", "mmCP_CPF_STATUS"), ("GC", "mmGRBM_STATUS_SE0"),
         # ih.c: what the DPC may touch (navi10_ih_get_wptr's overflow clear, navi10_ih_set_rptr without a doorbell)
         ("OSSSYS", "mmIH_RB_CNTL"), ("OSSSYS", "mmIH_RB_RPTR"), ("OSSSYS", "mmIH_RB_WPTR"), ("OSSSYS", "mmIH_STATUS"),
         # dcn.c: the eight registers its decoded summary reads by name (ADR 0011 point 3), out of the 75 on
         # DCN_REGISTERS below.
         ("DMU", "mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS"), ("DMU", "mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"),
         ("DMU", "mmHUBPREQ0_DCSURF_SURFACE_PITCH"), ("DMU", "mmHUBP0_DCHUBP_CNTL"),
         ("DMU", "mmOTG0_OTG_CONTROL"), ("DMU", "mmOTG0_OTG_H_TOTAL"), ("DMU", "mmOTG0_OTG_V_TOTAL"),
         ("DMU", "mmOTG0_OTG_GLOBAL_SYNC_STATUS"),
         # dcn.c: DcnFlip's own registers (0.7.20, ADR 0011 point 3 step 2), the four read besides the six it
         # writes (the write six are named again in DCN_WRITE_REGISTERS below, offset() caches the IP map so
         # this costs nothing).
         ("DMU", "mmHUBPREQ0_DCSURF_SURFACE_INUSE"), ("DMU", "mmOTG0_OTG_STATUS_FRAME_COUNT"),
         ("DMU", "mmHUBPREQ0_DCSURF_FLIP_CONTROL"), ("DMU", "mmHUBPREQ0_DCSURF_SURFACE_CONTROL"),
         ("DMU", "mmOTG0_OTG_MASTER_UPDATE_LOCK"), ("DMU", "mmOTG0_OTG_TRIGA_MANUAL_TRIG"),
         # dcn.c: DcnEscape's vsync-interrupt-route diagnostics (docs/design/vsync-interrupt-route.md). Already
         # on DCN_REGISTERS (the _DCN_OTG list below) as part of the 75-register dump; named here as well so the
         # decoded summary can read it and log its own field (OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN, bit 31)
         # by name instead of pulling it back out of Regs[].
         ("DMU", "mmOTG0_OTG_VUPDATE_KEEPOUT")]

# 0.7.20, ADR 0011 point 3 step 2 (M87, HUBP0 only): the write side of DcnFlip's sequence, on its own allow
# list so that nothing outside this exact set can be written through the escape or the WDDM flip DDI. Every
# name is already on DCN_REGISTERS (the read table) below; MmioDcnWrite (mmio.c) checks this list, never the
# read one. 0.7.24 (ADR 0011 point 3 step 3) adds OTG0_OTG_GLOBAL_SYNC_STATUS for the hardware vsync interrupt's
# own enable/ack (dcn.c's DcnVsyncEnable, DcnVsyncInterrupt) - not part of the M87 flip sequence itself, but the
# same write-only-through-one-checked-table rule applies to it.
DCN_WRITE_REGISTERS = ["mmHUBPREQ0_DCSURF_SURFACE_PITCH", "mmOTG0_OTG_MASTER_UPDATE_LOCK", "mmHUBPREQ0_DCSURF_FLIP_CONTROL",
                       "mmHUBPREQ0_DCSURF_SURFACE_CONTROL", "mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH",
                       "mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS", "mmOTG0_OTG_TRIGA_MANUAL_TRIG",
                       "mmOTG0_OTG_GLOBAL_SYNC_STATUS",
                       # BD-013: AMD optc1_set_blank, used by dcn201_tg_funcs.
                       "mmOTG0_OTG_BLANK_CONTROL", "mmOTG0_OTG_DOUBLE_BUFFER_CONTROL",
                       # M15.14 (0.7.216.20): the plane's pixel format per flip (plane_format.h). AMD writes the same
                       # three for a format change: hubp1_program_pixel_format (SURFACE_PIXEL_FORMAT, crossbar),
                       # dpp201_cnv_setup (CNVC_SURFACE_PIXEL_FORMAT). The firmware's values with those fields
                       # replaced, inside the flip's OTG0 update lock, and only when the format changes.
                       "mmHUBP0_DCSURF_SURFACE_CONFIG", "mmHUBPRET0_HUBPRET_CONTROL",
                       "mmCNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT"]

# ADR 0011 point 3: the DCN 2.0.1 ("DMU") display controller's registers, read-only, the first step before any
# write to this block (docs/adr/0011-present-is-a-flip.md). HUBPREQn and HUBPn for n in 0..3 (one instance of
# each per scanout pipe), OTGm for m in 0..1 (the two timing generators), plus DCHUBBUB_CTRL_STATUS. DMU has
# exactly one IP_BASE instance on this part (cyan_skillfish_ip_offset.h): the HUBP/OTG numbering is part of the
# register name, not an instance selector, same as GC's SE/SH banked registers are not here either.
_DCN_HUBPREQ = ["DCSURF_PRIMARY_SURFACE_ADDRESS", "DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", "DCSURF_SURFACE_INUSE",
                "DCSURF_SURFACE_PITCH", "DCSURF_FLIP_CONTROL", "DCSURF_FLIP_CONTROL2", "DCSURF_SURFACE_CONTROL",
                "DCSURF_SURFACE_FLIP_INTERRUPT"]
_DCN_HUBP = ["DCHUBP_CNTL", "DCSURF_SURFACE_CONFIG", "DCSURF_ADDR_CONFIG", "DCSURF_TILING_CONFIG"]
_DCN_OTG = ["OTG_CONTROL", "OTG_MASTER_UPDATE_LOCK", "OTG_GLOBAL_CONTROL0", "OTG_GLOBAL_SYNC_STATUS",
            "OTG_VUPDATE_KEEPOUT", "OTG_H_TOTAL", "OTG_V_TOTAL", "OTG_STATUS", "OTG_STATUS_POSITION",
            "OTG_STATUS_FRAME_COUNT", "OTG_VERTICAL_INTERRUPT0_CONTROL", "OTG_VUPDATE_PARAM", "OTG_TRIGA_MANUAL_TRIG"]
DCN_REGISTERS = ([f"mmHUBPREQ{n}_{r}" for n in range(4) for r in _DCN_HUBPREQ] +
                 [f"mmHUBP{n}_{r}" for n in range(4) for r in _DCN_HUBP] +
                 [f"mmOTG{m}_{r}" for m in range(2) for r in _DCN_OTG] +
                 ["mmDCHUBBUB_CTRL_STATUS"])

# Internal observation only: keep the existing 75-register escape payload stable.
# AMD hubp1_is_flip_pending / optc1_get_crtc_scanoutpos (MIT).
DCN_PRIVATE_READ_REGISTERS = ["mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE",
    "mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", "mmOTG0_OTG_V_BLANK_START_END"]
# BD-018: reference-derived readback; no timing/modeset writes.
DCN_TIMING_READ_REGISTERS = ["mmOTG0_OTG_H_BLANK_START_END", "mmDP_DTO0_PHASE", "mmDP_DTO0_MODULO",
    "mmOTG0_PIXEL_RATE_CNTL", "mmOTG0_OTG_INTERLACE_CONTROL", "mmOTG0_OTG_V_TOTAL_CONTROL"]
DCN_PRIVATE_READ_REGISTERS += DCN_TIMING_READ_REGISTERS + ["mmOTG0_OTG_BLANK_CONTROL", "mmOTG0_OTG_DOUBLE_BUFFER_CONTROL"]
# M15.14 (0.7.216.20): the plane's pixel format registers (plane_format.h), read at start to decode the firmware's
# format and on restore to verify it; MPCC0_MPCC_CONTROL, CNVC_CFG0_FORMAT_CONTROL and CNVC_CFG0_ALPHA_2BIT_LUT
# for the start log only (the blend mode, ALPHA_EN and the 2-bit alpha table decide whether a surface's alpha
# reaches the output; CNVC_UPDATE_PENDING is the double-buffer witness of the CNVC fields). HUBP0_DCSURF_SURFACE_CONFIG is on DCN_REGISTERS already; named here for a define.
DCN_PRIVATE_READ_REGISTERS += ["mmHUBP0_DCSURF_SURFACE_CONFIG", "mmHUBPRET0_HUBPRET_CONTROL",
    "mmCNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT", "mmCNVC_CFG0_FORMAT_CONTROL", "mmMPCC0_MPCC_CONTROL",
    "mmCNVC_CFG0_ALPHA_2BIT_LUT"]
NAMED += [("DMU", name) for name in DCN_PRIVATE_READ_REGISTERS + ["mmOTG0_OTG_STATUS_POSITION", "mmOTG0_OTG_GLOBAL_CONTROL0"]]
NAMED += [("CLK", "mmCLK4_0_CLK4_CLK2_CURRENT_CNT")]

BAR5_LENGTH = 0x80000     # BC250_BAR5_LENGTH in mmio.c

# IPs gen_probes.SPEC does not carry a header for, because they are deliberately not part of the diagusb read
# sweep (third_party/linux-amdgpu/PROVENANCE.md: "the display headers ... are deliberately not part of the
# general MMIO read sweep"). gen_regs.py's own DCN table is not that sweep - it is its own generated table,
# through the same regcalc mechanism - so it gets its header here instead of by adding DMU to SPEC.
EXTRA_HEADERS = {"DMU": "dcn_2_0_1_offset.h", "THM": "thm_10_0_offset.h", "CLK": "clk_11_0_1_offset.h",
                 # BD-056: amdgpu defines the Cyan Skillfish golden TSC registers inside gfx_v10_0.c itself (no
                 # smuio header carries the _Cyan_Skillfish names), so regcalc reads them from the vendored file;
                 # the SMUIO segment bases come from cyan_skillfish_ip_offset.h as for every other IP.
                 "SMUIO": ROOT / "driver/amdgpu-import/reference/gfx_v10_0.c"}
# M438/E30: 72 pre-amdgpu reads match the live SMN-backed k10temp sensor.
# This is a read-only addition, not a permission for raw SMU messages.
EXTRA_READS = [("THM", "mmTHM_TCON_CUR_TMP"), ("CLK", "mmCLK4_0_CLK4_CLK2_CURRENT_CNT")]
# BD-056, wddm.c CalibrateGpuClock: the 100 MHz SMUIO counter amdgpu's gfx_v10_0_get_gpu_clock_counter() reads on
# GC 10.1.3 (gfx_v10_0.c:7694-7706), read only. On the escape read list too, so that `bc250kmd_cli read` can show
# it moving on the lab before anything relies on it. Unit A's Linux AMDGPU_INFO_TIMESTAMP, which amdgpu answers
# from these two registers, returned 0x00000043_A80556B6 (evidence/linux/2026-09-21-E13-reference-2/
# boot4-readonly-after-windows/info.txt).
TSC_REGISTERS = [("SMUIO", "mmGOLDEN_TSC_COUNT_UPPER_Cyan_Skillfish"), ("SMUIO", "mmGOLDEN_TSC_COUNT_LOWER_Cyan_Skillfish")]
EXTRA_READS += TSC_REGISTERS
NAMED += TSC_REGISTERS
# Keep the timing inputs observable through READ_REG for pre-deployment control.
EXTRA_READS += [("DMU", name) for name in DCN_TIMING_READ_REGISTERS + ["mmOTG0_OTG_V_BLANK_START_END"]]

# ---- DP audio (dpaudio.c): step 0 observation, step 1 endpoint presence, step 2 stream ----------------------------
# Step 0, read only, and on the READ_REG list as well so that `bc250kmd_cli read <name>` shows each one: the codec's
# root and function parameters, the audio straps, the DCCG audio DTOs, and for each of the two stream encoders the
# DIG/DP/AFMT/HPD state that says which encoder drives the monitor and what the firmware left in its audio path.
# Linux read the same registers on unit A at the same regcalc offsets (facts M820, evidence/linux/2026-10-07-L1007-
# dp-audio); none of them is on the read-side-effect list of facts M25. Linux's dce_audio.c, dcn10_stream_encoder.c
# and dcn10_link_encoder.c (v6.18) read or write every one of them.
AUDIO_READ_REGISTERS = (
    ["mmAZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID", "mmAZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID",
     "mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES", "mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_STREAM_FORMATS",
     "mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES", "mmDC_PINSTRAPS",
     "mmDCCG_AUDIO_DTO_SOURCE", "mmDCCG_AUDIO_DTO0_PHASE", "mmDCCG_AUDIO_DTO0_MODULE",
     "mmDCCG_AUDIO_DTO1_PHASE", "mmDCCG_AUDIO_DTO1_MODULE"] +
    [f"mm{r.format(n=n)}" for n in range(2) for r in (
        "DIG{n}_DIG_FE_CNTL", "DIG{n}_DIG_BE_CNTL", "DP{n}_DP_VID_STREAM_CNTL", "DP{n}_DP_SEC_CNTL",
        "DP{n}_DP_SEC_AUD_N", "DP{n}_DP_SEC_AUD_M_READBACK", "DP{n}_DP_SEC_TIMESTAMP", "DIG{n}_AFMT_CNTL",
        "DIG{n}_AFMT_AUDIO_SRC_CONTROL", "DIG{n}_AFMT_AUDIO_PACKET_CONTROL", "DIG{n}_AFMT_AUDIO_PACKET_CONTROL2",
        "DIG{n}_AFMT_STATUS", "HPD{n}_DC_HPD_INT_STATUS",
        # Step 2: the two AFMT registers enc1_se_setup_dp_audio (dcn10_stream_encoder.c) writes beside the ones above.
        "DIG{n}_AFMT_INFOFRAME_CONTROL0", "DIG{n}_AFMT_60958_0")])
# Step 2's first write: the AFMT (HDMIn) memories in the DIO. dcn201_init_hw writes DIO_MEM_PWR_CTRL 0 ("power AFMT
# HDMI memory", dcn201_hwseq.c); the firmware leaves every HDMIn_MEM_PWR_FORCE at 3 (0x6DB6D800 on unit A, r19, while
# DP audio played at 0.33x and silent; Linux read 0 there and played at 1.0x, L1007b).
AUDIO_READ_REGISTERS += ["mmDIO_MEM_PWR_CTRL"]
EXTRA_READS += [("DMU", name) for name in AUDIO_READ_REGISTERS]
# The Azalia controller block (dce_dc_hda_azf0controller_dispdec), the DIO memory power and the DCCG clock gates:
# read only, so that `bc250kmd_cli read <name>` can compare them with Linux. Linux read all of them on unit A before,
# during and after a playback that sounded at the right rate (L1007b, scratch/linux-1007b, 2026-10-07), while
# Windows r19 consumed DP audio at 0.33x with every register of AUDIO_READ_REGISTERS equal to Linux. Plain MMIO; the
# endpoint indirect space (INDEX/DATA) stays out: a CPU sweep of its indices hung unit A under Linux in L1007b.
AUDIO_CONTROLLER_READ_REGISTERS = [
    "mmAZALIA_CONTROLLER_CLOCK_GATING", "mmAZALIA_AUDIO_DTO", "mmAZALIA_AUDIO_DTO_CONTROL", "mmAZALIA_SOCCLK_CONTROL",
    "mmAZALIA_UNDERFLOW_FILLER_SAMPLE", "mmAZALIA_DATA_DMA_CONTROL", "mmAZALIA_BDL_DMA_CONTROL",
    "mmAZALIA_RIRB_AND_DP_CONTROL", "mmAZALIA_CORB_DMA_CONTROL", "mmAZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER",
    "mmAZALIA_CYCLIC_BUFFER_SYNC", "mmAZALIA_GLOBAL_CAPABILITIES", "mmAZALIA_OUTPUT_PAYLOAD_CAPABILITY",
    "mmAZALIA_OUTPUT_STREAM_ARBITER_CONTROL", "mmDIO_MEM_PWR_STATUS", "mmDIO_MEM_PWR_CTRL2",
    "mmDIO_MEM_PWR_CTRL3", "mmDCCG_GATE_DISABLE_CNTL", "mmDCCG_GATE_DISABLE_CNTL2"]
EXTRA_READS += [("DMU", name) for name in AUDIO_CONTROLLER_READ_REGISTERS]
# Step 2 also reads the DP reference clock counter (facts M788, 100 kHz units), from which the DTO1 module is set.
# It is on the READ_REG list already (EXTRA_READS above); here it joins the audio read table, read only.
AUDIO_OTHER_READS = [("CLK", "mmCLK4_0_CLK4_CLK2_CURRENT_CNT")]
# The two Azalia endpoints' INDEX/DATA pairs (dce_audio.c:55-84 write_indirect_azalia_reg / read_indirect_azalia_reg).
# Not on the READ_REG list: what DATA returns depends on what INDEX holds, so only dpaudio.c reaches them, and only
# for the indices below. The INDEX write of an indirect READ selects a configuration register and changes nothing
# else; it is on the audio write table for that purpose. A DATA write is admitted only for AUDIO_IX_WRITE.
AUDIO_ENDPOINT_PAIRS = [(f"mmAZF0ENDPOINT{e}_AZALIA_F0_CODEC_ENDPOINT_INDEX",
                         f"mmAZF0ENDPOINT{e}_AZALIA_F0_CODEC_ENDPOINT_DATA") for e in range(2)]
# Step 1's two direct writes, both from dce_aud_hw_init (dce_audio.c:1260-1295): the rate capabilities and the
# CLKSTOP/EPSS bits of the codec's function group. Both are on AUDIO_READ_REGISTERS, so each write is verifiable.
AUDIO_DIRECT_WRITES = ["mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES",
                       "mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES"]
# Step 2's direct writes, the stream half: exactly the registers dce_aud_wall_dto_setup (dce_audio.c, the DP branch:
# DTO1 and the DTO source select) and enc1_se_dp_audio_setup, enc1_se_dp_audio_enable, enc1_se_dp_audio_disable and
# enc1_se_audio_mute_control (dcn10_stream_encoder.c, which DCN 2.0.1 uses) write, for both stream encoders. All are
# on AUDIO_READ_REGISTERS, so each write is read back. DTO0, DP_SEC_AUD_M_READBACK and AFMT_STATUS stay read only.
AUDIO_DIRECT_WRITES += ["mmDCCG_AUDIO_DTO_SOURCE", "mmDCCG_AUDIO_DTO1_PHASE", "mmDCCG_AUDIO_DTO1_MODULE"]
AUDIO_DIRECT_WRITES += [f"mm{r.format(n=n)}" for n in range(2) for r in (
    "DP{n}_DP_SEC_CNTL", "DP{n}_DP_SEC_AUD_N", "DP{n}_DP_SEC_TIMESTAMP", "DIG{n}_AFMT_CNTL",
    "DIG{n}_AFMT_AUDIO_SRC_CONTROL", "DIG{n}_AFMT_AUDIO_PACKET_CONTROL", "DIG{n}_AFMT_AUDIO_PACKET_CONTROL2",
    "DIG{n}_AFMT_INFOFRAME_CONTROL0", "DIG{n}_AFMT_60958_0")]
AUDIO_DIRECT_WRITES += ["mmDIO_MEM_PWR_CTRL"]
# Indirect indices, by their ENDPOINT0 name in dcn_2_0_1_offset.h (regcalc: `lookup ix...`); the ENDPOINT1 name must
# carry the same number, which main() checks. Read: the configuration and status registers dpaudio.c observes. The
# interrupt-status indices (AUDIO_ENABLED_INT_STATUS and its neighbours) are left out: whether a read clears them is
# not known. Write: exactly the registers dce_aud_hw_init, dce_aud_az_configure, dce_aud_az_enable and
# dce_aud_az_disable write. ACP_DATA, which dce_aud_az_configure also writes through dce_11_0_d.h (index 0x27), has no
# index in the DCN 2.0.1 header, so it is on neither list (test_regcalc.py pins its absence).
_AZ = "ixAZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_"
AUDIO_IX_WRITE = ([_AZ + "CHANNEL_SPEAKER"] + [f"{_AZ}AUDIO_DESCRIPTOR{i}" for i in range(14)] +
                  [_AZ + r for r in ("RESPONSE_LIPSYNC", "RESPONSE_HBR")] + [f"{_AZ}SINK_INFO{i}" for i in range(9)] +
                  [_AZ + "HOT_PLUG_CONTROL"])
AUDIO_IX_READ = AUDIO_IX_WRITE + [_AZ + r for r in ("UNSOLICITED_RESPONSE", "RESPONSE_PIN_SENSE", "WIDGET_CONTROL",
                                                    "RESPONSE_CONFIGURATION_DEFAULT")]

LINE = re.compile(r"^([A-Z0-9]+)\.(\S+) (0x[0-9a-f]+)$")


def regmap(maps, ip):
    if ip not in maps:
        header = next((h for i, h, _ in SPEC if i == ip), None) or EXTRA_HEADERS.get(ip)
        if header is None:
            sys.exit(f"no header known for IP {ip} (neither gen_probes.SPEC nor gen_regs.EXTRA_HEADERS)")
        maps[ip] = RegMap(ip=ip, reg_header=HDR_DIR / header)
    return maps[ip]


def offset(maps, ip, name):
    return regmap(maps, ip).byte_offset(name)


def az_index(maps, name):
    """The indirect index of an ENDPOINT0 ix name; the ENDPOINT1 name must define the same number."""
    rm = regmap(maps, "DMU")
    other = name.replace("ixAZF0ENDPOINT0_", "ixAZF0ENDPOINT1_")
    if name not in rm.ix or other not in rm.ix:
        sys.exit(f"{name}: no such indirect index for both endpoints in the DMU header")
    if rm.index(name) != rm.index(other):
        sys.exit(f"{name}: endpoint 0 has 0x{rm.index(name):X}, endpoint 1 0x{rm.index(other):X}")
    return rm.index(name)


def main():
    maps = {}
    reads = sorted({int(m.group(3), 16) for m in map(LINE.match, (ROOT / "tools/win/bc250rd/reglist.txt")
                                                     .read_text(encoding="utf-8").splitlines()) if m})
    reads = sorted(set(reads) | {offset(maps, ip, name) for ip, name in EXTRA_READS})
    if reads and reads[-1] >= BAR5_LENGTH:
        sys.exit(f"read list reaches 0x{reads[-1]:X}, beyond the 0x{BAR5_LENGTH:X} bytes of BAR5 that mmio.c maps")
    writes = [(offset(maps, ip, name), ip, name, exp, why) for ip, name, exp, why in WRITABLE]
    for off, ip, name, _, _ in writes:
        if off not in reads:
            sys.exit(f"{ip}.{name} is writable but not on the read list: a write must be verifiable")

    out = ["// Generated by gen_regs.py from the vendored amdgpu headers through tools/regcalc. Do not edit.",
           "#pragma once", ""]
    audio_pairs = [("DMU", reg) for pair in AUDIO_ENDPOINT_PAIRS for reg in pair]
    for ip, name in dict.fromkeys(NAMED + EXTRA_READS + audio_pairs):
        out.append(f"#define BC250_REG_{ip}_{name[2:]} 0x{offset(maps, ip, name):05X}ul")
    # DP audio: the Azalia endpoint's indirect indices (dpaudio.c), by name, from dcn_2_0_1_offset.h's ix defines.
    out += ["", "// Azalia endpoint indirect indices (ixAZF0ENDPOINTn_AZALIA_F0_CODEC_PIN_CONTROL_*, the same for n = 0, 1):",
            "// the value written into AZF0ENDPOINTn_AZALIA_F0_CODEC_ENDPOINT_INDEX, never a BAR5 offset."]
    for name in AUDIO_IX_READ:
        out.append(f"#define BC250_AZ_IX_{name[len(_AZ):]} 0x{az_index(maps, name):04X}ul")
    # The READ_REG names that are not in tools/win/bc250rd/reglist.txt, for `bc250kmd_cli read <name>`: X(ip, name,
    # offset), the name without its mm prefix. reglist.txt's own names stay with bc250rd.
    out += ["", "// The named positive controls of the READ_REG list (EXTRA_READS), for bc250kmd_cli's `read <name>`.",
            "#define BC250_REG_READ_NAMES(X) \\"]
    out += [f"    X(\"{ip}\", \"{name[2:]}\", 0x{offset(maps, ip, name):05X}ul) \\" for ip, name in dict.fromkeys(EXTRA_READS)]
    out += ["    /* end */", ""]
    # Not gated like the tables below: bc250kmd_escape.h's BC250_DCN_REG_COUNT (the escape struct's fixed array)
    # is checked against this one at compile time in dcn.c, which does not define BC250_REGS_WITH_TABLES.
    out.append(f"#define BC250_DCN_REG_INFO_COUNT {len(DCN_REGISTERS)}")
    out += ["", "// The tables are for mmio.c alone; everybody else gets the names.", "#ifdef BC250_REGS_WITH_TABLES",
            "", "// Offsets that may be written through the escape, with the experiment that allowed each."]
    out.append(f"#define BC250_MMIO_WRITE_ALLOW_COUNT {len(writes)}")
    out.append("static const unsigned long g_MmioWriteAllow[BC250_MMIO_WRITE_ALLOW_COUNT] = {")
    for off, ip, name, exp, why in sorted(writes):
        out.append(f"    0x{off:05X}ul,   // {ip}.{name[2:]}  {exp}: {why}")
    out += ["};", "", "// Escape reads: bc250rd/reglist.txt plus named positive controls (EXTRA_READS). Sorted, unique.",
            f"#define BC250_MMIO_READ_ALLOW_COUNT {len(reads)}",
            "static const unsigned long g_MmioReadAllow[BC250_MMIO_READ_ALLOW_COUNT] = {"]
    out += ["    " + ", ".join(f"0x{o:05X}" for o in reads[i:i + 10]) + "," for i in range(0, len(reads), 10)]
    out += ["};", ""]
    summary = []
    sequenced = set()
    for table, exp, match, until, read_only in SEQUENCES:
        entries = {}
        windows = until if isinstance(until, list) else [(0.0, until)]
        traced = [a for since, to in windows for a in accesses(TRACE, match, reads=isinstance(until, list), since=since, until=to)]
        for _, kind, name, off, _ in traced:
            ip, reg = name.split(".", 1)
            if offset(maps, ip, "mm" + reg) != off:
                sys.exit(f"{name}: the trace has 0x{off:05X}, regcalc says 0x{offset(maps, ip, 'mm' + reg):05X}")
            entries[off] = name
        for ip, reg, *note in read_only:
            entries[offset(maps, ip, reg)] = f"{ip}.{reg[2:]} ({note[0] if note else 'read only'})"
        if max(entries) >= BAR5_LENGTH:
            sys.exit(f"{table}: 0x{max(entries):X} is beyond BAR5")
        when = ", ".join(f"{a} to {b} s" for a, b in until) if isinstance(until, list) else f"first {until} s"
        out += [f"// {exp}: what amdgpu wrote on unit A in this step (E03 trace, {when}, names matching",
                f"// {match}), plus the registers it only polled. For the kernel command alone.",
                f"#define BC250_MMIO_{table.upper()}_ALLOW_COUNT {len(entries)}",
                f"static const unsigned long g_Mmio{table}Allow[BC250_MMIO_{table.upper()}_ALLOW_COUNT] = {{"]
        out += [f"    0x{off:05X}ul,   // {entries[off]}" for off in sorted(entries)]
        out += ["};", ""]
        sequenced |= set(entries)
        summary.append(f"{len(entries)} in the {table} sequence")

    # ADR 0011 point 3: the DCN dump's own table, not a trace (there is none yet under Windows for this IP) but
    # every offset still only from regcalc, over DCN_REGISTERS. Two views of the same 75 registers: sorted and
    # unique for MmioDcnRead's table check (dcn_allow), and in DCN_REGISTERS's fixed order with names for
    # MmioDcnTable/dcn.c's dump (dcn_named).
    dcn_named = [(offset(maps, "DMU", name), name[2:]) for name in DCN_REGISTERS]
    dcn_allow = sorted({off for off, _ in dcn_named})
    if len(dcn_allow) != len(dcn_named):
        sys.exit("DCN_REGISTERS has two names for the same offset: gen_regs.py assumed they are all distinct")
    dcn_allow = sorted(set(dcn_allow) | {offset(maps, "DMU", name) for name in DCN_PRIVATE_READ_REGISTERS})
    if dcn_allow[-1] >= BAR5_LENGTH:
        sys.exit(f"dcn: 0x{dcn_allow[-1]:X} is beyond BAR5")
    out += ["// ADR 0011 point 3: the DCN dump's own registers (gen_regs.py's DCN_REGISTERS, tools/regcalc, ip DMU).",
            "// Plus internal observation registers; sorted, unique, for MmioDcnRead's table check.",
            f"#define BC250_MMIO_DCN_ALLOW_COUNT {len(dcn_allow)}",
            "static const unsigned long g_MmioDcnAllow[BC250_MMIO_DCN_ALLOW_COUNT] = {"]
    out += ["    " + ", ".join(f"0x{o:05X}" for o in dcn_allow[i:i + 10]) + "," for i in range(0, len(dcn_allow), 10)]
    if len(dcn_named) != len(DCN_REGISTERS):
        sys.exit("BC250_DCN_REG_INFO_COUNT was emitted before dcn_named was built: they must have the same length")
    out += ["};", "",
            "// The same registers, in DCN_REGISTERS's order, with their names: what MmioDcnTable() hands dcn.c.",
            "static const BC250_DCN_REG_INFO g_DcnRegisters[BC250_DCN_REG_INFO_COUNT] = {"]
    out += [f'    {{ "{name}", 0x{off:05X}ul }},' for off, name in dcn_named]
    out += ["};", ""]
    summary.append(f"{len(dcn_named)} in the DCN dump")

    # 0.7.20: DcnFlip's write allow list. Every offset must already be on dcn_allow (the read table) above -
    # DcnFlip reads a register before it ever writes it (M87's sequence), so a write-only register here would
    # be a register this driver never proved readable under Windows.
    dcn_write = sorted({offset(maps, "DMU", name) for name in DCN_WRITE_REGISTERS})
    if len(dcn_write) != len(DCN_WRITE_REGISTERS):
        sys.exit("DCN_WRITE_REGISTERS has two names for the same offset: gen_regs.py assumed they are all distinct")
    for off in dcn_write:
        if off not in dcn_allow:
            sys.exit(f"dcn write: 0x{off:05X} is not on DCN_REGISTERS (the read table) - add it there first")
    out += ["// 0.7.20, ADR 0011 point 3 step 2 (M87, HUBP0 only): DcnFlip's write allow list (gen_regs.py's",
            "// DCN_WRITE_REGISTERS). Sorted, unique, checked by MmioDcnWrite; every offset is also in g_MmioDcnAllow.",
            f"#define BC250_MMIO_DCN_WRITE_ALLOW_COUNT {len(dcn_write)}",
            "static const unsigned long g_MmioDcnWriteAllow[BC250_MMIO_DCN_WRITE_ALLOW_COUNT] = {"]
    out += ["    " + ", ".join(f"0x{o:05X}" for o in dcn_write[i:i + 10]) + "," for i in range(0, len(dcn_write), 10)]
    out += ["};", ""]
    summary.append(f"{len(dcn_write)} in the DCN flip's write list")

    for ip, name in NAMED:
        if offset(maps, ip, name) not in reads and offset(maps, ip, name) not in sequenced and offset(maps, ip, name) not in dcn_allow:
            sys.exit(f"{ip}.{name} is used by the driver but neither on the read list, a sequence's table nor the DCN table")
    out += ["#endif", ""]

    # DP audio: dpaudio.c's own tables, in a block of their own so that only dpaudio.c compiles them (mmio.c's
    # BC250_REGS_WITH_TABLES block above is unchanged by this feature).
    audio_read = sorted({offset(maps, "DMU", n) for n in AUDIO_READ_REGISTERS} |
                        {offset(maps, ip, n) for ip, n in audio_pairs} |
                        {offset(maps, ip, n) for ip, n in AUDIO_OTHER_READS})
    audio_write = sorted({offset(maps, ip, n) for ip, n in audio_pairs} |
                         {offset(maps, "DMU", n) for n in AUDIO_DIRECT_WRITES})
    if len(audio_read) != len(AUDIO_READ_REGISTERS) + len(audio_pairs) + len(AUDIO_OTHER_READS):
        sys.exit("AUDIO_READ_REGISTERS, the endpoint pairs and AUDIO_OTHER_READS have two names for one offset")
    if len(audio_write) != len(audio_pairs) + len(AUDIO_DIRECT_WRITES):
        sys.exit("AUDIO_DIRECT_WRITES and the endpoint pairs have two names for one offset")
    for ip, n in AUDIO_OTHER_READS:
        if offset(maps, ip, n) in audio_write:
            sys.exit(f"{ip}.{n} is read only for DP audio and must not be on the audio write table")
    for off in audio_write:
        if off not in audio_read:
            sys.exit(f"audio write: 0x{off:05X} is not on the audio read table - a write must be verifiable")
    if audio_read[-1] >= BAR5_LENGTH:
        sys.exit(f"audio: 0x{audio_read[-1]:X} is beyond BAR5")
    ix_read = sorted({az_index(maps, n) for n in AUDIO_IX_READ})
    ix_write = sorted({az_index(maps, n) for n in AUDIO_IX_WRITE})
    if len(ix_read) != len(AUDIO_IX_READ) or len(ix_write) != len(AUDIO_IX_WRITE):
        sys.exit("AUDIO_IX_READ or AUDIO_IX_WRITE has two names for one index")
    if not set(ix_write) <= set(ix_read):
        sys.exit("an indirect index is writable but not readable: a write must be verifiable")
    out += ["// DP audio (dpaudio.c): direct reads (AUDIO_READ_REGISTERS, the endpoint INDEX/DATA pairs and the DP reference",
            "// clock counter), direct writes (the pairs, dce_aud_hw_init's two function-group registers and step 2's DTO,",
            "// AFMT and DP_SEC registers), and the indirect indices dpaudio.c may read and write through a pair. Sorted,",
            "// unique. For dpaudio.c alone.",
            "#ifdef BC250_REGS_WITH_AUDIO_TABLES",
            f"#define BC250_MMIO_AUDIO_ALLOW_COUNT {len(audio_read)}",
            "static const unsigned long g_MmioAudioAllow[BC250_MMIO_AUDIO_ALLOW_COUNT] = {"]
    out += ["    " + ", ".join(f"0x{o:05X}" for o in audio_read[i:i + 10]) + "," for i in range(0, len(audio_read), 10)]
    out += ["};", f"#define BC250_MMIO_AUDIO_WRITE_ALLOW_COUNT {len(audio_write)}",
            "static const unsigned long g_MmioAudioWriteAllow[BC250_MMIO_AUDIO_WRITE_ALLOW_COUNT] = {"]
    out += ["    " + ", ".join(f"0x{o:05X}" for o in audio_write[i:i + 10]) + "," for i in range(0, len(audio_write), 10)]
    out += ["};", f"#define BC250_AZ_IX_READ_ALLOW_COUNT {len(ix_read)}",
            "static const unsigned long g_AzIxReadAllow[BC250_AZ_IX_READ_ALLOW_COUNT] = {"]
    out += ["    " + ", ".join(f"0x{o:04X}" for o in ix_read[i:i + 10]) + "," for i in range(0, len(ix_read), 10)]
    out += ["};", f"#define BC250_AZ_IX_WRITE_ALLOW_COUNT {len(ix_write)}",
            "static const unsigned long g_AzIxWriteAllow[BC250_AZ_IX_WRITE_ALLOW_COUNT] = {"]
    out += ["    " + ", ".join(f"0x{o:04X}" for o in ix_write[i:i + 10]) + "," for i in range(0, len(ix_write), 10)]
    out += ["};", "#endif", ""]
    summary.append(f"{len(audio_read)} audio reads, {len(audio_write)} audio writes, "
                   f"{len(ix_read)}/{len(ix_write)} Azalia indices read/write")
    (HERE / "regs.generated.h").write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"{len(reads)} readable, {len(writes)} writable: " + ", ".join(f"{n[2:]}=0x{o:05X}" for o, _, n, _, _ in writes)
          + "; " + ", ".join(summary))


if __name__ == "__main__":
    main()
