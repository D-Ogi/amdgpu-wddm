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
      ("GC", "mmGRBM_STATUS2", "read observation for opt-in RLC reload reset, M349"),
      ("GC", "mmGRBM_SOFT_RESET", "not traced: AMD RLC reset callback, opt-in M349"),
      ("GC", "mmRLC_SAFE_MODE", "not traced: AMD paired safe-mode scope, M370"),
      ("GC", "mmSDMA0_FREEZE", "not traced: AMD queue quiescence, M370"),
      ("GC", "mmSDMA1_FREEZE", "not traced: AMD queue quiescence, M370"),
      ("GC", "mmSDMA0_STATUS1_REG", "not traced: AMD idle fallback, M370"),
      ("GC", "mmSDMA1_STATUS1_REG", "not traced: AMD idle fallback, M370")]
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
         # gfx.c: are the engines halted?
         ("GC", "mmCP_ME_CNTL"), ("GC", "mmCP_MEC_CNTL"), ("GC", "mmSDMA0_F32_CNTL"), ("GC", "mmSDMA1_F32_CNTL"),
         # gfx.c: a stage that stopped half way must not leave a me/pipe/queue selected
         ("GC", "mmGRBM_GFX_CNTL"),
         # gfx.c: the one write that passes a stopped sequence (gpumem.c flushes the TLB after its binds)
         ("MMHUB", "mmMMVM_INVALIDATE_ENG17_SEM"),
         # gfx.c: a PLAN answers the GRBM CAM probe, which writes one of these and reads the other
         ("GC", "mmVGT_ESGS_RING_SIZE"), ("GC", "mmVGT_ESGS_RING_SIZE_UMD"),
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
                       "mmOTG0_OTG_BLANK_CONTROL", "mmOTG0_OTG_DOUBLE_BUFFER_CONTROL"]

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
NAMED += [("DMU", name) for name in DCN_PRIVATE_READ_REGISTERS + ["mmOTG0_OTG_STATUS_POSITION", "mmOTG0_OTG_GLOBAL_CONTROL0"]]
NAMED += [("CLK", "mmCLK4_0_CLK4_CLK2_CURRENT_CNT")]

BAR5_LENGTH = 0x80000     # BC250_BAR5_LENGTH in mmio.c

# IPs gen_probes.SPEC does not carry a header for, because they are deliberately not part of the diagusb read
# sweep (third_party/linux-amdgpu/PROVENANCE.md: "the display headers ... are deliberately not part of the
# general MMIO read sweep"). gen_regs.py's own DCN table is not that sweep - it is its own generated table,
# through the same regcalc mechanism - so it gets its header here instead of by adding DMU to SPEC.
EXTRA_HEADERS = {"DMU": "dcn_2_0_1_offset.h", "THM": "thm_10_0_offset.h", "CLK": "clk_11_0_1_offset.h"}
# M438/E30: 72 pre-amdgpu reads match the live SMN-backed k10temp sensor.
# This is a read-only addition, not a permission for raw SMU messages.
EXTRA_READS = [("THM", "mmTHM_TCON_CUR_TMP"), ("CLK", "mmCLK4_0_CLK4_CLK2_CURRENT_CNT")]
# Keep the timing inputs observable through READ_REG for pre-deployment control.
EXTRA_READS += [("DMU", name) for name in DCN_TIMING_READ_REGISTERS + ["mmOTG0_OTG_V_BLANK_START_END"]]

LINE = re.compile(r"^([A-Z0-9]+)\.(\S+) (0x[0-9a-f]+)$")


def offset(maps, ip, name):
    if ip not in maps:
        header = next((h for i, h, _ in SPEC if i == ip), None) or EXTRA_HEADERS.get(ip)
        if header is None:
            sys.exit(f"no header known for IP {ip} (neither gen_probes.SPEC nor gen_regs.EXTRA_HEADERS)")
        maps[ip] = RegMap(ip=ip, reg_header=HDR_DIR / header)
    return maps[ip].byte_offset(name)


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
    for ip, name in dict.fromkeys(NAMED + EXTRA_READS):
        out.append(f"#define BC250_REG_{ip}_{name[2:]} 0x{offset(maps, ip, name):05X}ul")
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
    (HERE / "regs.generated.h").write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"{len(reads)} readable, {len(writes)} writable: " + ", ".join(f"{n[2:]}=0x{o:05X}" for o, _, n, _, _ in writes)
          + "; " + ", ".join(summary))


if __name__ == "__main__":
    main()
