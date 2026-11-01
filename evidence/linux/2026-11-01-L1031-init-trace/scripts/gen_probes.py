#!/usr/bin/env python3
"""Generate the kprobe definitions of the init trace (development PC, before step i1).

    python gen_probes.py --offsets <i0 dir>/offsets.json --out <dir>

Writes into <dir>: probes.txt (lines for /sys/kernel/tracing/kprobe_events), filters.txt ('<event> <filter>'),
init-regs.json (the dword offsets the lab-side helpers and the decoders use).

Rules the probe lines follow, from measurements on this kernel (6.18.52-0-lts):
- amdgpu is not loaded when they are written, so no $argN fetch: the kernel refuses it for a module that is not
  loaded yet (E13 boot 9, evidence/linux/2026-09-21-E13-reference-2/boot9-third-reload/README.txt). Arguments are
  read from the x86-64 argument registers at function entry (%di, %si, %dx, %cx, %r8), memory through +OFF(...).
  $retval in return probes is accepted (same README; E29 used it).
- Offsets inside structs of the firmware ABI (psp_gfx_if.h) or fixed by the declaration order of simple structs
  (amdgpu_ib, amdgpu_ip_block, psp_context.adev) are taken from the headers; the one offset that depends on the
  kernel configuration, amdgpu_device.rmmio, comes from offsets.json (ko_offsets.py: module BTF and the machine code
  of amdgpu_device_rreg/wreg must agree). Without an agreed rmmio no state probe reads a register: the probes are
  then plain markers.
- A state probe reads registers only through amdgpu's own mapping (adev->rmmio + byte offset), each a single 32-bit
  load, the same access readl() makes; a NULL rmmio (before ioremap) makes the fetch fail harmlessly (an address
  below TASK_SIZE is refused by copy_from_kernel_nofault).
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cparse  # noqa: E402
import watch  # noqa: E402
from regnames import Names  # noqa: E402

GROUP = "bcinit"
IP_BLOCK_ADEV = 16      # struct amdgpu_ip_block {status (5 x bool, padded to 8); *version; *adev} (amdgpu.h:399)
PSP_ADEV = 0            # struct psp_context {struct amdgpu_device *adev; ...} (amdgpu_psp.h:360)
# struct amdgpu_ib {*sa_bo @0; u32 length_dw @8; u64 gpu_addr @16; u32 *ptr @24; u32 flags @32} (amdgpu_ring.h:99)
IB_LEN, IB_VA, IB_PTR, IB_FLAGS = 8, 16, 24, 32
IB_DWORDS = 64
RING_ADEV = 0           # struct amdgpu_ring {struct amdgpu_device *adev; ...} (amdgpu_ring.h:299)
SMU_ADEV = 0            # struct smu_context {struct amdgpu_device *adev; ...} (amdgpu_smu.h:542)

# (event, function, adev expression or None, extra fetch args, return probe?)
# Functions reached through a function pointer (IP-block, RLC, ring and KIQ callbacks) cannot be inlined at that
# call, so their entry probes always fire; for gfx_v10_0_rlc_resume (also called directly) a missing hit is no
# negative witness (E28 lesson). kprobe_profile after the run says which ones fired.
PROBES = [
    ("ipadd", "amdgpu_device_ip_block_add", "%di", "", False),
    ("psp_hw", "psp_hw_init", f"+{IP_BLOCK_ADEV}(%di)", "", True),
    ("smu_hw", "smu_hw_init", f"+{IP_BLOCK_ADEV}(%di)", "", True),
    ("dm_hw", "dm_hw_init", f"+{IP_BLOCK_ADEV}(%di)", "", True),
    ("gfx_hw", "gfx_v10_0_hw_init", f"+{IP_BLOCK_ADEV}(%di)", "", True),
    ("rlc_resume", "gfx_v10_0_rlc_resume", "%di", "", True),
    ("kiq_res", "gfx10_kiq_set_resources", f"+{RING_ADEV}(%di)", "mask=%si:x64", False),
    ("kiq_map", "gfx10_kiq_map_queues", f"+{RING_ADEV}(%di)", "ring=%si:x64", False),
    ("en_kcq", "amdgpu_gfx_enable_kcq", "%di", "", True),
    ("ring_test", "amdgpu_ring_test_helper", f"+{RING_ADEV}(%di)", "ring=%di:x64", True),
    ("sdma_hw", "sdma_v5_0_hw_init", f"+{IP_BLOCK_ADEV}(%di)", "", True),
    ("gfx_late", "gfx_v10_0_late_init", f"+{IP_BLOCK_ADEV}(%di)", "", True),
    ("ib_tests", "amdgpu_ib_ring_tests", "%di", "", True),
    ("psp_cmd", "psp_cmd_submit_buf", f"+{PSP_ADEV}(%di)",
     # struct psp_gfx_cmd_resp (psp_gfx_if.h:446): cmd_id @8, command union @28; load_ip_fw = addr lo/hi, size,
     # fw_type; setup_tmr = addr lo/hi, size, flags, system addr lo/hi; load_toc = addr lo/hi, size.
     # struct amdgpu_firmware_info: ucode_id is the first member (amdgpu_ucode.h). The ucode pointer is NULL for
     # commands without firmware; the ucode_id fetch then fails and its value is to be ignored (decoder does).
     "ucode=%si:x64 uid=+0(%si):u32 id=+8(%dx):x32 c0=+28(%dx):x32 c1=+32(%dx):x32 c2=+36(%dx):x32 "
     "c3=+40(%dx):x32 c4=+44(%dx):x32 c5=+48(%dx):x32 fence=%cx:x64", True),
    ("db32", "amdgpu_mm_wdoorbell", None, "idx=%si:u32 v=%dx:x32", False),
    ("db64", "amdgpu_mm_wdoorbell64", None, "idx=%si:u32 v=%dx:x64", False),
    ("ib_sched", "amdgpu_ib_schedule", None,
     f"ring=%di:x64 n=%si:u32 len=+{IB_LEN}(%dx):u32 va=+{IB_VA}(%dx):x64 flags=+{IB_FLAGS}(%dx):x32 "
     f"dw=+0(+{IB_PTR}(%dx)):x32[{IB_DWORDS}]", False),
    ("smumsg", "smu_cmn_send_smc_msg_with_param", f"+{SMU_ADEV}(%di)", "msg=%si:u32 param=%dx:x32", True),
    ("copy_buf", "amdgpu_copy_buffer", None, "ring=%di:x64 src=%si:x64 dst=%dx:x64 bytes=%cx:u32", True),
    ("st_w", "amdgpu_device_wreg", "%di", "reg=%si:x32 val=%dx:x32", False),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--offsets", required=True, help="offsets.json from ko_offsets.py")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    off = json.loads(Path(args.offsets).read_text())
    names = Names()
    rmmio = off.get("agreed_rmmio")
    syms = off.get("symbols", {})
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    snap = [(tag, reg, names.byte(reg)) for tag, reg in watch.SNAPSHOT]
    lines, notes = [], []
    for ev, fn, adev, extra, ret in PROBES:
        state = syms.get(fn, "unknown")
        if state != "present":
            notes.append(f"# skipped {ev}: symbol {fn} is {state} in the module (ko_offsets.py)")
            continue
        fetch = []
        if adev and rmmio is not None:
            fetch = [f"{tag}=+{byte:#x}(+{rmmio:#x}({adev})):x32" for tag, _, byte in snap]
        elif ev == "st_w":
            notes.append("# skipped st_w: no agreed amdgpu_device.rmmio offset, a write probe without state is noise")
            continue
        body = " ".join(x for x in [extra] + fetch if x)
        lines.append(f"p:{GROUP}/{ev} amdgpu:{fn} {body}".rstrip())
        if ret:
            lines.append(f"r:{GROUP}/{ev}_ret amdgpu:{fn} rc=$retval:s32")
    trig = [names.dword(r) for r in watch.TRIGGERS]
    filters = []
    if any(l.startswith(f"p:{GROUP}/st_w ") for l in lines):
        filters.append("st_w " + " || ".join(f"reg == {d:#x}" for d in trig))
    (out / "probes.txt").write_text("\n".join(notes + lines) + "\n", newline="\n")
    (out / "filters.txt").write_text("\n".join(filters) + ("\n" if filters else ""), newline="\n")
    regs = {
        "snapshot": [[tag, reg, byte] for tag, reg, byte in snap],
        "triggers": {r: names.dword(r) for r in watch.TRIGGERS},
        "psp": {k: names.dword(v) for k, v in watch.PSP.items()},
        "smu": {k: names.dword(v) for k, v in watch.SMU.items()},
        "smu_msg": {v: k for k, v in cparse.defines(cparse.LINUX / "pm/swsmu/inc/pmfw_if/smu_v11_8_ppsmc.h",
                                                    r"PPSMC_MSG_\w+").items()},
        "rmmio": rmmio,
        "generated_from": {"offsets": str(Path(args.offsets).resolve()), "module_sha256": off.get("module_sha256")},
    }
    (out / "init-regs.json").write_text(json.dumps(regs, indent=1), newline="\n")
    n_state = sum(1 for l in lines if rmmio is not None and f"(+{rmmio:#x}(" in l)
    print(f"{len(lines)} probe lines ({n_state} with register state), {len(notes)} skipped, "
          f"{len(filters)} filter(s), rmmio={'%#x' % rmmio if rmmio is not None else 'none'} -> {out}")
    for n in notes:
        print(n)


if __name__ == "__main__":
    main()
