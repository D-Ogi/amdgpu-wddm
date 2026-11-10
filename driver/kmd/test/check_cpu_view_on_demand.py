#!/usr/bin/env python3
"""Source gate of BD-210 G1 and BD-352: CpuVisibleOnDemand on the UMD and the type-0 LB7A allocations (driver/kmd).

The rule (cpu_view_policy.h Bc250CpuViewOnDemandAllowed, host-tested by cpu_view_policy_test.c and umd_blob_test.c)
is only half of the change. This gate checks the other half in the source, where no host test reaches:

  1. WddmCreateAdmit sets FlagsWddm2.CpuVisibleOnDemand from UmdBlobAllocCpuVisibleOnDemand(&view,
     placement.supported, aperture set), behind the start's latch wddm->CpuVisibleOnDemand, after CpuVisible and
     before the admission returns.
  2. WddmStart latches the latch from GuardCpuVisibleOnDemand().
  3. GuardCpuVisibleOnDemand returns TRUE in the release flavour: the lab selector cannot reach a release build.
  4. The selector name LabCpuVisibleOnDemand is only in lab-flavour code (guard.c, entry.c), so the release SYS
     does not hold it (tools/quality/flavour_gate.json LABCPUVISIBLEONDEMAND checks the built file).
  5. The type-0 LB7A create sets the flag through Bc250CpuViewOnDemandAllowed with the record class
     (Bc250CpuViewClassOfRecord of the resource record), its CpuVisible bit and its placement, behind the same latch.
  6. UmdBlobAllocCpuVisibleOnDemand answers through Bc250CpuViewOnDemandAllowed: one rule, not two copies.

Usage: check_cpu_view_on_demand.py [--repo DIR] [--negative-control]
The negative control applies one source mutation per check; each must fail its check, else the gate fails.
"""
import argparse
import pathlib
import re
import sys

LAB_IF = re.compile(r"^\s*#\s*(if\s+defined\s*\(\s*AMDGPU_WDDM_LAB_FLAVOUR\s*\)|ifdef\s+AMDGPU_WDDM_LAB_FLAVOUR\b)")
ANY_IF = re.compile(r"^\s*#\s*if")
ELSE = re.compile(r"^\s*#\s*(else|elif)\b")
ENDIF = re.compile(r"^\s*#\s*endif\b")
SELECTOR = "LabCpuVisibleOnDemand"


def lab_lines(text):
    """The set of 1-based line numbers that only the lab flavour compiles."""
    stack, lab = [], set()
    for number, line in enumerate(text.splitlines(), 1):
        if LAB_IF.match(line):
            stack.append(True)
            continue
        if ANY_IF.match(line):
            stack.append(None)
            continue
        if ELSE.match(line) and stack:
            stack[-1] = False if stack[-1] else stack[-1]
            continue
        if ENDIF.match(line) and stack:
            stack.pop()
            continue
        if any(s is True for s in stack):
            lab.add(number)
    return lab


def function_body(text, signature):
    at = text.find(signature)
    if at < 0:
        return None
    open_at = text.find("{", at)
    depth = 0
    for i in range(open_at, len(text)):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[open_at:i + 1]
    return None


def check(sources):
    """[failure text] for the sources {name: text}."""
    failures = []
    wddm, guard, entry = sources["wddm.c"], sources["guard.c"], sources["entry.c"]

    visible = "info->FlagsWddm2.CpuVisible = (UINT)UmdBlobAllocCpuVisible(&view);"
    at = wddm.find(visible)
    end = wddm.find("return BC250_CREATE_STEP_ADMITTED;", at) if at >= 0 else -1
    span = wddm[at:end] if at >= 0 and end >= 0 else ""
    m = re.search(r"info->FlagsWddm2\.CpuVisibleOnDemand\s*=([^;]*);", span)
    umd_call = re.compile(r"UmdBlobAllocCpuVisibleOnDemand\(\s*&view\s*,\s*placement\.supported\s*,\s*"
                          r"BC250_WDDM_SEGMENT_SET\(\s*BC250_WDDM_SEGMENT_APERTURE\s*\)\s*\)")
    if not m or not umd_call.search(m.group(1)) or "wddm->CpuVisibleOnDemand" not in m.group(1):
        failures.append("1: WddmCreateAdmit does not set CpuVisibleOnDemand from UmdBlobAllocCpuVisibleOnDemand(&view, "
                        "placement.supported, aperture set) behind wddm->CpuVisibleOnDemand, after CpuVisible")

    # 5 (BD-352): the type-0 LB7A site, after the placement and the CpuVisible bit, through the shared rule, with the
    # record class that Bc250WddmCreateAllocation derives once from the resource record.
    cached = "info->FlagsWddm2.Cached = policy.Cached;"
    at = wddm.find(cached)
    end = wddm.find("return BC250_CREATE_STEP_ADMITTED;", at) if at >= 0 else -1
    span = wddm[at:end] if at >= 0 and end >= 0 else ""
    lb7a = re.search(r"if\s*\(\s*!gdiType\s*&&\s*\(\s*wddm\s*==\s*NULL\s*\|\|\s*wddm->CpuVisibleOnDemand\s*\)\s*&&\s*"
                     r"Bc250CpuViewOnDemandAllowed\(\s*call->CpuViewClass\s*,\s*\(int\)policy\.CpuVisible\s*,\s*"
                     r"info->SupportedReadSegmentSet\s*,\s*BC250_WDDM_SEGMENT_SET\(\s*BC250_WDDM_SEGMENT_APERTURE\s*\)\s*\)"
                     r"\s*\)\s*\{\s*info->FlagsWddm2\.CpuVisibleOnDemand\s*=\s*1;", span)
    placed = wddm.rfind("info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(info->PreferredSegment.SegmentId0);",
                        0, at if at >= 0 else 0)
    if not lb7a or placed < 0 or \
            not re.search(r"call\.CpuViewClass\s*=\s*Bc250CpuViewClassOfRecord\(\s*pCreateAllocation->pPrivateDriverData"
                          r"\s*,\s*pCreateAllocation->PrivateDriverDataSize\s*\)", wddm):
        failures.append("5: the type-0 LB7A create does not set CpuVisibleOnDemand through Bc250CpuViewOnDemandAllowed "
                        "with the record class, its CpuVisible and its placement, behind wddm->CpuVisibleOnDemand")

    # 6 (BD-352): one rule. The UMD answer goes through the same Bc250CpuViewOnDemandAllowed, not a copy of it.
    blob = function_body(sources["umd_blob.c"], "int UmdBlobAllocCpuVisibleOnDemand(")
    if not blob or not re.search(r"return\s+Bc250CpuViewOnDemandAllowed\(\s*UmdBlobCpuViewClass\(", blob):
        failures.append("6: UmdBlobAllocCpuVisibleOnDemand does not answer through Bc250CpuViewOnDemandAllowed")

    if "wddm->CpuVisibleOnDemand = GuardCpuVisibleOnDemand();" not in wddm:
        failures.append("2: WddmStart does not latch wddm->CpuVisibleOnDemand from GuardCpuVisibleOnDemand()")

    body = function_body(guard, "BOOLEAN GuardCpuVisibleOnDemand(void)")
    release = body.split("#else", 1)[1].split("#endif", 1)[0] if body and "#else" in body else ""
    if not re.fullmatch(r"\s*return TRUE;\s*", release):
        failures.append("3: GuardCpuVisibleOnDemand does not return TRUE in the release flavour")

    for name, text in (("guard.c", guard), ("entry.c", entry), ("wddm.c", wddm)):
        lab = lab_lines(text)
        for number, line in enumerate(text.splitlines(), 1):
            code = line.split("//", 1)[0]
            if (SELECTOR in code or "GuardLabReadCpuVisibleOnDemand(" in code) and number not in lab:
                failures.append("4: %s:%d names the lab selector outside lab-flavour code" % (name, number))
    if not any(SELECTOR in l for i, l in enumerate(guard.splitlines(), 1) if i in lab_lines(guard)):
        failures.append("4: guard.c reads no %s in lab-flavour code" % SELECTOR)
    return failures


def mutations(sources):
    """(name, check id, mutated sources): each must fail its own check."""
    def swap(name, old, new):
        if old not in sources[name]:
            raise SystemExit("negative control: '%s' is not in %s" % (old, name))
        out = dict(sources)
        out[name] = sources[name].replace(old, new, 1)
        return out
    yield "no gate on the latch", "1", swap("wddm.c", "(UINT)((wddm == NULL || wddm->CpuVisibleOnDemand) &&", "(UINT)((")
    yield "umd set not passed", "1", swap("wddm.c", "UmdBlobAllocCpuVisibleOnDemand(&view, placement.supported,",
                                          "UmdBlobAllocCpuVisibleOnDemand(&view, BC250_WDDM_SEGMENT_SET(1),")
    yield "lb7a without the latch", "5", swap("wddm.c", "if (!gdiType && (wddm == NULL || wddm->CpuVisibleOnDemand) &&",
                                              "if (!gdiType &&")
    yield "lb7a without the class", "5", swap("wddm.c", "Bc250CpuViewOnDemandAllowed(call->CpuViewClass,",
                                              "Bc250CpuViewOnDemandAllowed(BC250_CPU_VIEW_LB7A_PLAIN,")
    yield "lb7a with its own rule", "6", swap("umd_blob.c", "return Bc250CpuViewOnDemandAllowed(UmdBlobCpuViewClass(",
                                              "return (UmdBlobCpuViewClass(")
    yield "no latch", "2", swap("wddm.c", "wddm->CpuVisibleOnDemand = GuardCpuVisibleOnDemand();",
                                "wddm->CpuVisibleOnDemand = TRUE;")
    body = function_body(sources["guard.c"], "BOOLEAN GuardCpuVisibleOnDemand(void)")
    yield "release off", "3", swap("guard.c", body, body.replace("#else\n    return TRUE;", "#else\n    return FALSE;"))
    yield "selector in release", "4", swap("entry.c", "#else\n    GuardLog(\"kmd: BD-167 flavour release",
                                           "#else\n    GuardLabReadCpuVisibleOnDemand(DriverObject);\n"
                                           "    GuardLog(\"kmd: BD-167 flavour release")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=str(pathlib.Path(__file__).resolve().parents[3]))
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    kmd = pathlib.Path(args.repo) / "driver" / "kmd"
    sources = {n: (kmd / n).read_text(encoding="utf-8").replace("\r\n", "\n") for n in ("wddm.c", "guard.c", "entry.c", "umd_blob.c")}

    failures = check(sources)
    for f in failures:
        print("FAIL " + f)
    if failures:
        return 1
    print("PASS cpu view on demand: 6 source checks")
    if not args.negative_control:
        return 0
    bad = 0
    for name, wanted, mutated in mutations(sources):
        got = check(mutated)
        caught = any(f.startswith(wanted + ":") for f in got)
        print("%s negative control '%s': check %s %s" % ("PASS" if caught else "FAIL", name, wanted,
                                                         "fails" if caught else "does not fail"))
        bad += not caught
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
