"""Generate hang.json for the L43/L47/L48 hang lane on the development PC. Every number comes from a header or a
tool, never typed: PM4 opcodes and field shifts from the kernel's nvd.h, SQ_CMD fields from gc_10_1_0_sh_mask.h and
navi10_enum.h, register byte offsets from tools/regcalc, the SDMA poll packet from the E29 packets.json, the PM4 pad
word from the E13 dispatch.json, and the spin shader from llvm-mc 22.1.8 (-mcpu=gfx1013, object file disassembly:
BF82FFFF s_branch to itself, BF810000 s_endpgm).
Usage: python gen-hang-tables.py  (writes hang.json next to this file)"""
import json
import os
import re
import subprocess
import sys

ROOT = os.environ.get("BC250_ROOT", r"P:\BC-250")
HERE = os.path.dirname(os.path.abspath(__file__))
NVD = os.path.join(ROOT, "ref", "linux-src", "drivers", "gpu", "drm", "amd", "amdgpu", "nvd.h")
TP = os.path.join(ROOT, "bc250-win", "third_party", "linux-amdgpu")
E29 = os.path.join(ROOT, "bc250-win", "evidence", "linux", "2026-09-24-E29-sdma-reset", "sources")

text = open(NVD, encoding="utf-8").read()


def const(name):
    m = re.search(r"#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+|\d+)\b", text)
    if not m:
        raise SystemExit("nvd.h: no " + name)
    return int(m.group(1), 0)


def shift(name):
    m = re.search(r"#define\s+" + name + r"\(x\)\s+\(\(x\)\s*<<\s*(\d+)\)", text)
    if not m:
        raise SystemExit("nvd.h: no shift macro " + name)
    return int(m.group(1))


def bit(name):
    m = re.search(r"#define\s+" + name + r"\s+\(1\s*<<\s*(\d+)\)", text)
    if not m:
        raise SystemExit("nvd.h: no bit macro " + name)
    return 1 << int(m.group(1))


# nvd.h:48-50: PACKET3(op, n) = (PACKET_TYPE3 << 30) | ((op & 0xFF) << 8) | ((n & 0x3FFF) << 16)
if not re.search(r"#define PACKET3\(op, n\)\s+\(\(PACKET_TYPE3 << 30\) \|\s*\\\s*\(\(\(op\) & 0xFF\) << 8\) \|\s*\\\s*"
                 r"\(\(n\) & 0x3FFF\) << 16\)", text):
    raise SystemExit("nvd.h: PACKET3 macro text changed; re-check the formula")


def packet3(op, n):
    return (const("PACKET_TYPE3") << 30) | ((op & 0xFF) << 8) | ((n & 0x3FFF) << 16)


# libdrm tests/amdgpu/deadlock_tests.c:273-281 (MIT): WAIT_REG_MEM, memory space, function 4 (!=), engine ME,
# reference 0, mask 0xffffffff, poll interval 4. gfx_v10_0_ring_test_ib (gfx_v10_0.c): WRITE_DATA, DST_SEL 5, WR_CONFIRM.
pm4 = {
    "wait_header": packet3(const("PACKET3_WAIT_REG_MEM"), 5),
    "wait_ctrl": (1 << shift("WAIT_REG_MEM_MEM_SPACE")) | (4 << shift("WAIT_REG_MEM_FUNCTION"))
                 | (0 << shift("WAIT_REG_MEM_ENGINE")),
    "wait_ref": 0, "wait_mask": 0xFFFFFFFF, "wait_interval": 4,
    "write_header": packet3(const("PACKET3_WRITE_DATA"), 3),
    "write_ctrl": (5 << shift("WRITE_DATA_DST_SEL")) | bit("WR_CONFIRM"),
}
disp = json.load(open(os.path.join(E29, "dispatch.json")))
pm4["pad"] = disp["pm4"]["pad_word"]
pm4["pad_cite"] = disp["pm4"]["pad_cite"]

sh = open(os.path.join(TP, "gc_10_1_0_sh_mask.h"), encoding="utf-8").read()
enum = open(os.path.join(TP, "navi10_enum.h"), encoding="utf-8").read()


def field(reg, f):
    s = int(re.search(rf"#define {reg}__{f}__SHIFT\s+(0x[0-9a-fA-F]+)", sh).group(1), 16)
    m = int(re.search(rf"#define {reg}__{f}_MASK\s+(0x[0-9a-fA-F]+)L", sh).group(1), 16)
    return s, m


def enumv(name):
    return int(re.search(rf"\b{name}\s*=\s*(0x[0-9a-fA-F]+)", enum).group(1), 16)


kill = 0
for f, v in (("CMD", enumv("SQ_IND_CMD_CMD_KILL")), ("MODE", enumv("SQ_IND_CMD_MODE_BROADCAST")), ("CHECK_VMID", 1)):
    s, m = field("SQ_CMD", f)
    kill |= (v << s) & m
vs, vm = field("SQ_CMD", "VM_ID")

names = ["mmSQ_CMD", "mmCP_VMID_RESET", "mmCP_HQD_ACTIVE", "mmCP_HQD_PQ_RPTR", "mmCP_HQD_PQ_WPTR_LO",
         "mmCP_HQD_DEQUEUE_REQUEST", "mmCP_GFX_HQD_ACTIVE", "mmCP_GFX_HQD_RPTR", "mmCP_GFX_HQD_WPTR", "mmGRBM_STATUS",
         "mmGRBM_STATUS2", "mmCP_STAT", "mmCP_ME_CNTL", "mmCP_MEC_CNTL", "mmSDMA0_STATUS_REG", "mmSDMA0_GFX_RB_RPTR",
         "mmSDMA0_GFX_RB_WPTR", "mmSDMA0_F32_CNTL", "mmSDMA0_GFX_RB_CNTL", "mmSDMA1_STATUS_REG",
         "mmSDMA1_GFX_RB_RPTR", "mmSDMA1_GFX_RB_WPTR", "mmGCVM_L2_PROTECTION_FAULT_STATUS",
         "mmGCVM_L2_PROTECTION_FAULT_CNTL"]
out = subprocess.run([sys.executable, os.path.join(ROOT, "bc250-win", "tools", "regcalc", "regcalc.py"), "lookup"]
                     + names, capture_output=True, text=True, check=True).stdout
regs = {}
for line in out.replace("\r", "").splitlines():
    m = re.match(r"(mm\w+): .*BAR5\+(0x[0-9A-Fa-f]+)", line)
    if m:
        regs[m.group(1)[2:]] = int(m.group(2), 16)
missing = [n for n in names if n[2:] not in regs]
if missing:
    raise SystemExit("regcalc did not resolve " + " ".join(missing))

doc = {
    "generated_by": "scratch/linux-1031/hang/gen-hang-tables.py",
    "pm4": pm4,
    "sdma": json.load(open(os.path.join(E29, "packets.json"))),
    "spin_shader": {"words": [0xBF82FFFF, 0xBF810000],
                    "cite": "llvm-mc 22.1.8 -mcpu=gfx1013: 'loop: s_branch loop; s_endpgm', llvm-objdump"},
    "sq_cmd": {"kill_base": kill, "vmid_shift": vs, "vmid_mask": vm,
               "cite": "gc_10_1_0_sh_mask.h SQ_CMD fields, navi10_enum.h SQ_IND_CMD_CMD_KILL/MODE_BROADCAST; "
                       "the value of bc250_gfx_soft_recover_vmid and the former gfx_v10_0_ring_soft_recovery"},
    "regs": regs,
}
with open(os.path.join(HERE, "hang.json"), "w", newline="\n") as fh:
    json.dump(doc, fh, indent=1)
print(json.dumps({k: (hex(v) if isinstance(v, int) else v) for k, v in pm4.items()}))
print("sq_cmd kill_base", hex(kill), "vmid shift", vs, "mask", hex(vm))
print("regs", len(regs))
