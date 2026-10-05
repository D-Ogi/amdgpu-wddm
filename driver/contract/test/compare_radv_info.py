#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Compare the caps this contract derives against what RADV derives on the real device.

Both sides are produced by the SAME function - Mesa's ac_print_gpu_info() - so this is a
field-by-field diff of one struct radeon_info against another, not a diff of two formats:

  reference   evidence/linux/2026-09-21-E14-vulkan-compute-reference/radv-info.txt
              RADV_DEBUG=info vulkaninfo --summary on unit A. radv_physical_device.c:2903 calls
              ac_print_gpu_info() when that flag is set.
  ours        bc250_caps_test.exe --radeon-info, which runs the blob through the real ac_* path
              and then calls the same ac_print_gpu_info().

Why this exists: a reversed conclusion about gb_addr_config survived two written reports and a
133-check test, because every check agreed with the blob and the blob agreed with the mistake.
Nothing inside the test could catch that. This compares against the device.

Usage:
    python compare_radv_info.py [--ours ours.txt] [--reference radv-info.txt] [--exe path]

With no arguments it runs bc250_caps_test.exe --radeon-info itself and compares against the
committed evidence copy. Exit status 0 when the only differences are the documented ones below.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]          # ...\bc250-win
DEFAULT_REFERENCE = (REPO / "evidence" / "linux" / "2026-09-21-E14-vulkan-compute-reference" /
                     "radv-info.txt")
# BC250_ROOT is the workspace root; by default the parent directory of this repository.
ROOT = Path(os.environ.get("BC250_ROOT", str(REPO.parent)))
DEFAULT_EXE = ROOT / "scratch" / "contract" / "bc250_caps_test.exe"

# Keys that are expected to differ, each with the reason. Anything NOT in here must match.
#
# Keeping this list short and justified is the point: every entry is a claim that a difference is
# structural rather than a bug, and a reviewer can check each one. Do not add a key here to make
# the script quiet.
EXPECTED_DIFFERENT = {
    "dev_filename":
        "the Linux device node (/dev/dri/renderD128). ac_print_gpu_info() prints it by doing "
        "readlink(/proc/self/fd/N); there is no fd and no such path here, so our side omits the "
        "line entirely. A property of the capture, not of the GPU.",
    "marketing_name":
        "on Linux this comes from libdrm's amdgpu.ids data file, keyed on device id and revision "
        "(ac_gpu_info.c:1615 ac_drm_get_marketing_name), NOT from any ioctl. There is no kernel "
        "source for it, so a WDDM driver has to supply the string itself. Worth a blob field if a "
        "UMD ever wants to print it.",
    "kernel_has_modifiers":
        "DRM format modifiers for KMS framebuffers (ac_gpu_info.c:1629, drmGetCap "
        "DRM_CAP_ADDFB2_MODIFIERS). There is no KMS on Windows and no WDDM equivalent, so our "
        "side leaves it 0. Not applicable rather than wrong.",
    "has_timeline_syncobj":
        "ac_gpu_info.c:1620 sets it from ac_drm_device_get_sync_provider(dev)->timeline_wait. "
        "That is a property of the winsys's synchronisation backend (DRM syncobj), not of the "
        "GPU and not of the blob. The WDDM equivalent is a monitored fence, and whether to "
        "advertise it is our winsys's decision to make later.",
    "max_alignment":
        "set only in ac_surface.c:1064, from addrlib's AddrGetMaxAlignments(). It never passes "
        "through ac_query_gpu_info() at all. This test deliberately does not link addrlib (see "
        "README, 'Not done'), so our side leaves it 0. The device's 65536 is a fact about "
        "addrlib on this chip, and it is the first thing the future ac_surface comparison should "
        "reproduce.",
}

# max_submitted_ibs for the five IP types this part does not have. Listed key by key rather than
# by an "IP" prefix, so that a real disagreement about GFX, COMPUTE, SDMA, VCN_JPEG or VPE still
# shows up as a mismatch.
_ABSENT_IP_MAX_IBS = (
    "max_submitted_ibs for an IP type this part does not have. The blob carries the kernel's 49, "
    "radv-info.txt shows 1. The kernel side is settled: amdgpu_kms.c:1325-1333 answers "
    "AMDGPU_INFO_MAX_IBS by looping amdgpu_ring_max_ibs(type) over every AMDGPU_HW_IP_*, a "
    "static per-type table with no per-boot state and no check for whether the IP exists - so 49 "
    "is what the ioctl returns on any boot, and re-capturing would not change it. The 1 comes "
    "from Mesa 26.1.6's own derivation for IPs with no queues (Mesa 26.1.6 side, source not "
    "checked: only main is checked out in ref\\mesa). The blob keeps the kernel's values, which "
    "is what a WDDM KMD would have to supply. Every IP that exists agrees to the digit: GFX 192, "
    "COMPUTE 125, SDMA 49, VCN_JPEG 16, VPE 49."
)
for _ip in ("UVD", "VCE", "UVD_ENC", "VCN_DEC", "VCN_ENC"):
    EXPECTED_DIFFERENT[f"IP[{_ip}].max_submitted_ibs"] = _ABSENT_IP_MAX_IBS

# Fields that exist in one Mesa's ac_print_gpu_info() and not the other's. The lab ran Mesa
# 26.1.6; this test links whatever is checked out in ref\mesa (currently a 2026-09-20 main
# checkout, i.e. NEWER). Each entry below was confirmed by grepping ref\mesa's ac_gpu_info.c for
# the name: absent means upstream removed or added the field, present-but-not-printed would mean
# something else and is NOT covered here.
MESA_VERSION_SKEW = {
    "mesh_fast_launch_2": "printed by 26.1.6, absent from ref\\mesa's ac_gpu_info.c: removed upstream",
    "uses_kernel_cu_mask": "printed by 26.1.6, absent from ref\\mesa's ac_gpu_info.c: removed upstream",
    "has_cs_regalloc_hang_bug": "added upstream after 26.1.6",
    "has_lbpw_tcs_wg_bug": "added upstream after 26.1.6",
    "has_smem_with_null_prt_bug":
        "added upstream after 26.1.6. Note for later: when it is 1, ac_gpu_info.c:1817-1827 makes "
        "ac_query_gpu_info() issue an extra amdgpu_sw_info_address_prt_wa_control_bit query and "
        "FAIL the whole probe if it errors. A future Mesa bump turns that into a blob requirement.",
    "high_va_max": "added upstream after 26.1.6 (the value itself is measured and asserted by the test)",
    "high_va_offset": "added upstream after 26.1.6 (likewise)",
    "virtual_address_alignment": "added upstream after 26.1.6 (likewise)",
}

# Differences that are NOT understood yet. Listed so they are reported loudly and separately, and
# they still fail the run: a comparison that goes green while a measurement disagrees is exactly
# the failure mode this script exists to prevent. Empty is the goal, not the assumption.
UNRESOLVED = {}
UNRESOLVED_NOTE = ""

# Section headers in ac_print_gpu_info()'s output. Used to report where a difference sits.
SECTION_RE = re.compile(r"^(\S[^=]*):\s*$")

# "    key = value", the overwhelming majority of lines.
KV_RE = re.compile(r"^\s{4}([A-Za-z_][A-Za-z_0-9\[\]]*)\s*=\s*(.*?)\s*$")

# "GB_ADDR_CONFIG: 0x00100044" - a bare header line that carries a value.
BARE_RE = re.compile(r"^([A-Z_][A-Z_0-9]*):\s*(\S+)\s*$")

# "    pci (domain:bus:dev.func): 0000:01:00.0"
PCI_RE = re.compile(r"^\s{4}(pci \(domain:bus:dev\.func\)):\s*(\S+)\s*$")

# "    IP GFX     10.1 \tqueues:1 \talign:256 \tpad_dw:0x7"
IP_VER_RE = re.compile(r"^\s{4}IP (\S+)\s+(\d+\.\d+)\s+queues:(\d+)\s+align:(\d+)\s+pad_dw:(\S+)\s*$")

# "    IP GFX     max_submitted_ibs = 192"
IP_IBS_RE = re.compile(r"^\s{4}IP (\S+)\s+max_submitted_ibs = (\d+)\s*$")

# "    cu_mask[SE0][SA0] = 0x3f \t(6)\tCU_EN = 0x3f"
CU_MASK_RE = re.compile(r"^\s{4}(cu_mask\[SE\d+\]\[SA\d+\]) = (\S+)\s+\((\d+)\)\s+CU_EN = (\S+)\s*$")

# Where ac_print_gpu_info()'s output stops and vulkaninfo's begins in the reference file.
STOP_MARKERS = ("Modifiers (32bpp):", "==========")


def parse(text, name):
    """Return ({key: (value, section, lineno)}, [warnings])."""
    fields, warnings, section = {}, [], "(top)"

    def put(key, value, lineno):
        if key in fields:
            warnings.append(f"{name}: duplicate key {key!r} at line {lineno}")
        fields[key] = (value, section, lineno)

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip("\n")
        if any(line.startswith(m) for m in STOP_MARKERS):
            break
        if not line.strip():
            continue

        m = CU_MASK_RE.match(line)
        if m:
            put(m.group(1), m.group(2), lineno)
            put(m.group(1) + ".count", m.group(3), lineno)
            put(m.group(1) + ".CU_EN", m.group(4), lineno)
            continue
        m = IP_VER_RE.match(line)
        if m:
            ip = m.group(1)
            put(f"IP[{ip}].version", m.group(2), lineno)
            put(f"IP[{ip}].queues", m.group(3), lineno)
            put(f"IP[{ip}].align", m.group(4), lineno)
            put(f"IP[{ip}].pad_dw", m.group(5), lineno)
            continue
        m = IP_IBS_RE.match(line)
        if m:
            put(f"IP[{m.group(1)}].max_submitted_ibs", m.group(2), lineno)
            continue
        m = PCI_RE.match(line)
        if m:
            put(m.group(1), m.group(2), lineno)
            continue
        m = KV_RE.match(line)
        if m:
            put(m.group(1), m.group(2), lineno)
            continue
        m = BARE_RE.match(line)
        if m:
            put(m.group(1), m.group(2), lineno)
            continue
        m = SECTION_RE.match(line)
        if m:
            section = m.group(1)
            continue
        warnings.append(f"{name}: unparsed line {lineno}: {line!r}")
    return fields, warnings


def normalise(value):
    """Compare 0x3f with 0x3F and 8192 MB with 8192 MB, but never 0 with 0x0 by accident."""
    v = value.strip()
    token = v.split()[0] if v.split() else v
    try:
        n = int(token, 16) if token.lower().startswith("0x") else int(token)
    except ValueError:
        return v.lower()
    unit = v[len(token):].strip()
    return f"{n}{(' ' + unit) if unit else ''}"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ours", type=Path, help="a saved bc250_caps_test.exe --radeon-info dump")
    ap.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--verbose", action="store_true", help="also list every field that matched")
    args = ap.parse_args()

    if not args.reference.exists():
        print(f"reference not found: {args.reference}", file=sys.stderr)
        return 2

    if args.ours:
        ours_text = args.ours.read_text(encoding="utf-8", errors="replace")
        ours_from = str(args.ours)
    else:
        if not args.exe.exists():
            print(f"{args.exe} not found; run test\\run.ps1 first, or pass --ours",
                  file=sys.stderr)
            return 2
        proc = subprocess.run([str(args.exe), "--radeon-info"], capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"{args.exe} --radeon-info exited {proc.returncode}\n{proc.stderr}",
                  file=sys.stderr)
            return 2
        ours_text = proc.stdout
        ours_from = f"{args.exe} --radeon-info"

    ref, ref_warn = parse(args.reference.read_text(encoding="utf-8", errors="replace"), "reference")
    ours, ours_warn = parse(ours_text, "ours")

    print("=" * 78)
    print(" radeon_info: this contract vs unit A under Linux")
    print("=" * 78)
    print(f"  reference  {args.reference}")
    print(f"             ({len(ref)} fields)")
    print(f"  ours       {ours_from}")
    print(f"             ({len(ours)} fields)\n")

    for w in ref_warn + ours_warn:
        print(f"  NOTE {w}")
    if ref_warn or ours_warn:
        print()

    mismatches, expected, skew, unresolved, only_ref, only_ours, same = [], [], [], [], [], [], []

    def bucket(key):
        """Which documented list, if any, covers this key."""
        base = key.split(".")[0].split("[")[0]
        for table in (EXPECTED_DIFFERENT, MESA_VERSION_SKEW, UNRESOLVED):
            if key in table or base in table:
                return table
        return None

    for key in sorted(set(ref) | set(ours)):
        in_ref, in_ours = key in ref, key in ours
        table = bucket(key)
        if in_ref and in_ours and normalise(ref[key][0]) == normalise(ours[key][0]):
            same.append(key)
        elif table is EXPECTED_DIFFERENT:
            expected.append(key)
        elif table is MESA_VERSION_SKEW:
            skew.append(key)
        elif table is UNRESOLVED:
            unresolved.append(key)
        elif in_ref and in_ours:
            mismatches.append(key)
        elif in_ref:
            only_ref.append(key)
        else:
            only_ours.append(key)

    if mismatches:
        print("-" * 78)
        print(f" MISMATCHES ({len(mismatches)}) - the device and this contract disagree")
        print("-" * 78)
        for key in mismatches:
            rv, section, rl = ref[key]
            ov, _, _ = ours[key]
            print(f"  {key}")
            print(f"      section    {section}")
            print(f"      unit A     {rv}    (radv-info.txt:{rl})")
            print(f"      ours       {ov}")
        print()

    if only_ref:
        print("-" * 78)
        print(f" ONLY ON THE DEVICE ({len(only_ref)}) - a field RADV printed and we did not")
        print("-" * 78)
        print("  Usually a Mesa version difference: the lab ran Mesa 26.1.6 and this test links")
        print("  whatever is checked out in ref\\mesa. Check before assuming it is benign - a")
        print("  field can also be missing because a conditional in ac_print_gpu_info() took the")
        print("  other branch, which would mean a real difference in the derived caps.\n")
        for key in only_ref:
            print(f"  {key:<44} unit A: {ref[key][0]}  (:{ref[key][2]})")
        print()

    if only_ours:
        print("-" * 78)
        print(f" ONLY HERE ({len(only_ours)}) - a field we printed and the device did not")
        print("-" * 78)
        print("  Same causes, reversed: a newer Mesa in ref\\mesa, or a conditional that took a")
        print("  different branch here than on the device.\n")
        for key in only_ours:
            print(f"  {key:<44} ours: {ours[key][0]}")
        print()

    if unresolved:
        print("-" * 78)
        print(f" UNRESOLVED ({len(unresolved)}) - understood well enough to be sure it is not a")
        print("             regression, not well enough to call it expected")
        print("-" * 78)
        for key in unresolved:
            rv = ref[key][0] if key in ref else "(absent)"
            ov = ours[key][0] if key in ours else "(absent)"
            print(f"  {key:<40} unit A {rv:<8} ours {ov}")
        print()
        print(UNRESOLVED_NOTE)
        print()

    def explain(keys, table, title, blurb):
        if not keys:
            return
        print("-" * 78)
        print(f" {title} ({len(keys)})")
        print("-" * 78)
        if blurb:
            print(blurb + "\n")
        for key in keys:
            base = key if key in table else key.split(".")[0].split("[")[0]
            rv = ref[key][0] if key in ref else "(absent)"
            ov = ours[key][0] if key in ours else "(absent)"
            print(f"  {key}\n      unit A  {rv}\n      ours    {ov}")
            print(f"      why     {table[base]}")
        print()

    explain(expected, EXPECTED_DIFFERENT,
            "EXPECTED DIFFERENCES - structural, each with a reason", None)
    explain(skew, MESA_VERSION_SKEW, "MESA VERSION SKEW",
            "  The device ran Mesa 26.1.6; this test links ref\\mesa. Each name below was checked\n"
            "  against ref\\mesa's ac_gpu_info.c: it is printed by one version and absent from the\n"
            "  other's source, which is a version difference rather than a different branch taken.")

    if args.verbose and same:
        print("-" * 78)
        print(f" MATCHED ({len(same)})")
        print("-" * 78)
        for key in same:
            print(f"  {key:<44} {ref[key][0]}")
        print()

    print("=" * 78)
    print(f" {len(same)} matched, {len(mismatches)} mismatched, {len(only_ref)} only on the "
          f"device, {len(only_ours)} only here,")
    print(f" {len(expected)} expected, {len(skew)} Mesa version skew, {len(unresolved)} unresolved")
    print("=" * 78)

    if mismatches or only_ref or only_ours:
        print("\nFAIL: the derived caps differ from unit A. Each difference above is either a bug")
        print("in the blob, a bug in the winsys translation, or a Mesa version difference that")
        print("belongs in MESA_VERSION_SKEW with its reason.")
        return 1
    if unresolved:
        print(f"\nOPEN: {len(same)} fields match. {len(unresolved)} differences are documented")
        print("above as unresolved and are deliberately not excused: they affect IP types this")
        print("part does not have, so they block nothing, but they are still a disagreement")
        print("between two measurements and the script will keep saying so until one is settled.")
        return 1
    print("\nOK: every field RADV derives on unit A is derived identically from the blob.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
