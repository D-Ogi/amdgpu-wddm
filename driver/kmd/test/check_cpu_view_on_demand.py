#!/usr/bin/env python3
"""Source gate of BD-210 G1: CpuVisibleOnDemand on the UMD allocations (driver/kmd).

The rule (umd_blob.c UmdBlobAllocCpuVisibleOnDemand, host-tested by umd_blob_test.c) is only half of the change.
This gate checks the other half in the source, where no host test reaches:

  1. WddmCreateAdmit sets FlagsWddm2.CpuVisibleOnDemand from UmdBlobAllocCpuVisibleOnDemand(&view), behind the
     start's latch wddm->CpuVisibleOnDemand, after CpuVisible and before the admission returns.
  2. WddmStart latches the latch from GuardCpuVisibleOnDemand().
  3. GuardCpuVisibleOnDemand returns TRUE in the release flavour: the lab selector cannot reach a release build.
  4. The selector name LabCpuVisibleOnDemand is only in lab-flavour code (guard.c, entry.c), so the release SYS
     does not hold it (tools/quality/flavour_gate.json LABCPUVISIBLEONDEMAND checks the built file).

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
    if not m or "UmdBlobAllocCpuVisibleOnDemand(&view)" not in m.group(1) or \
            "wddm->CpuVisibleOnDemand" not in m.group(1):
        failures.append("1: WddmCreateAdmit does not set CpuVisibleOnDemand from "
                        "UmdBlobAllocCpuVisibleOnDemand(&view) behind wddm->CpuVisibleOnDemand, after CpuVisible")

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
    yield "no gate on the latch", "1", swap("wddm.c", "(wddm == NULL || wddm->CpuVisibleOnDemand) && ", "")
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
    sources = {n: (kmd / n).read_text(encoding="utf-8").replace("\r\n", "\n") for n in ("wddm.c", "guard.c", "entry.c")}

    failures = check(sources)
    for f in failures:
        print("FAIL " + f)
    if failures:
        return 1
    print("PASS cpu view on demand: 4 source checks")
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
