"""Find the label layout of fact M898 in the repository's WinForms code: a Label with a Dock and AutoSize off.

Label.OnTextChanged calls Label.AdjustSize. With AutoSize off it sets the size the program asked for, which for a
docked Label is 100x23, the default of a Label, and the parent docks it again right after. Between the two, the
parent paints the area that the small Label uncovers: the start window of tester.31 rc4 showed a partial picture of
its countdown that way (docs/facts M898). The control application holds the rule at run time (--smoke-labels in its
build). This gate reads the source of everything else: the installer's and the tools' PowerShell, and the C# of the
other WinForms programs (the setup window, the lab overlay), including C# inside PowerShell here-strings.

It finds, in every .ps1, .psm1 and .cs file outside evidence/ (copies of scripts as they ran, never run again):
  1. PowerShell: a variable made with New-Object (System.)Windows.Forms.Label or LinkLabel, or with [..Label]::new(),
     that gets a Dock other than None (an assignment or -Property) and never AutoSize = $true;
  2. C#: a new Label or LinkLabel whose initializer has a Dock other than DockStyle.None and no AutoSize = true, or a
     variable made with new Label that gets such a Dock later and never AutoSize = true.
Comments do not count. A Label made by a helper (the applications' Ui.Label sets AutoSize on) is not seen here.

ALLOWED lists the findings that are meant, each with its reason. An allowed finding that the scan no longer makes is
an error too, so the list cannot hold a file that has changed. The start window of 84f35234
(tools/release/installer/start-window.ps1 and start-confirm.ps1) draws its text itself and has no Label; the
controls (test_winforms_labels.py) check that the scan reads both files and finds nothing there.

Exit 0 with no findings outside ALLOWED and no stale entry in it, 1 otherwise.
"""
import argparse
import os
from pathlib import Path
import re
import sys

ALLOWED = {
    ("tools/release/test-session-checks.ps1", "$label"):
        "the session checks build the rc4 window, never shown, as the reference picture for the new one",
    ("tools/win/app-launch/label-window.ps1", "$label"):
        "the measurement window of M898 has the docked Label on purpose",
    ("tools/win/kmd-deploy/ops/heartbeat.ps1", "$l"):
        "a lab tool whose window exists so that DWM presents; a partial picture there harms no measurement",
    ("tools/win/amdgpu_wddm_control/src/LabelLayout.cs", "bad"):
        "the negative control of --smoke-labels, which that smoke must find",
}

SKIP_DIRS = {".git", "evidence"}
SUFFIXES = {".ps1", ".psm1", ".cs"}

# ---- PowerShell -------------------------------------------------------------------------------------------------
PS_LABEL = re.compile(
    r"\$(?P<var>[\w:]+)\s*=\s*(?:New-Object\s+(?:-TypeName\s+)?(?:System\.)?Windows\.Forms\.(?:Link)?Label\b"
    r"|\[(?:System\.)?Windows\.Forms\.(?:Link)?Label\]::new\()",
    re.I)
# Matched at the end of the New-Object: a -Property hashtable on the same line.
PS_PROPERTY = re.compile(r"[^\r\n]*?-Property\s*@\{", re.I)
PS_DOCK_VALUE = re.compile(r"\bDock\s*=\s*(?P<value>[^;\r\n}]+)", re.I)
PS_AUTOSIZE_TRUE = re.compile(r"\bAutoSize\s*=\s*\$true\b", re.I)
PS_BLOCK_COMMENT = re.compile(r"<#.*?#>", re.S)
PS_LINE_COMMENT = re.compile(r"(?m)^[ \t]*#[^\r\n]*")

# ---- C# -----------------------------------------------------------------------------------------------------------
CS_NEW_LABEL = re.compile(r"new\s+(?:System\.Windows\.Forms\.)?(?:Link)?Label\b\s*(?:\(\s*\))?\s*(?P<brace>\{)?")
CS_VAR_LABEL = re.compile(
    r"(?P<var>[A-Za-z_]\w*)\s*=\s*new\s+(?:System\.Windows\.Forms\.)?(?:Link)?Label\b")
CS_DOCKED = re.compile(r"\bDock\s*=\s*(?:System\.Windows\.Forms\.)?DockStyle\.(?!None\b)(?P<value>\w+)")
CS_AUTOSIZE_TRUE = re.compile(r"\bAutoSize\s*=\s*true\b")
CS_COMMENT = re.compile(r"//[^\r\n]*|/\*.*?\*/", re.S)


def blank(match):
    """The comment replaced with spaces, its line breaks kept, so offsets and line numbers stay."""
    return re.sub(r"[^\r\n]", " ", match.group(0))


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def balanced(text, start):
    """The text between the brace at start and its partner, or the rest of the text."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return text[start + 1:]


def is_none(value):
    return re.search(r"\bNone\b", value, re.I) is not None


def scan_powershell(text):
    """(line, variable, dock value) for every PowerShell Label variable that is docked without AutoSize."""
    code = PS_LINE_COMMENT.sub(blank, PS_BLOCK_COMMENT.sub(blank, text))
    labels = {}
    for m in PS_LABEL.finditer(code):
        var = m.group("var")
        entry = labels.setdefault(var.lower(), {"var": "$" + var, "line": line_of(code, m.start()), "dock": None, "auto": False})
        prop = PS_PROPERTY.match(code, m.end())
        if prop:
            body = balanced(code, prop.end() - 1)
            dock = PS_DOCK_VALUE.search(body)
            if dock and not is_none(dock.group("value")):
                entry["dock"] = dock.group("value").strip()
            if PS_AUTOSIZE_TRUE.search(body):
                entry["auto"] = True
    for entry in labels.values():
        name = re.escape(entry["var"])
        for d in re.finditer(name + r"\.Dock\s*=\s*(?P<value>[^;\r\n]+)", code, re.I):
            if not is_none(d.group("value")):
                entry["dock"] = entry["dock"] or d.group("value").strip()
        if re.search(name + r"\.AutoSize\s*=\s*\$true\b", code, re.I):
            entry["auto"] = True
    return [(e["line"], e["var"], e["dock"]) for e in labels.values() if e["dock"] and not e["auto"]]


def scan_csharp(text):
    """(line, name, dock value) for every C# Label that is docked without AutoSize, in an initializer or later."""
    code = CS_COMMENT.sub(blank, text)
    found = []
    variables = {}
    for m in CS_VAR_LABEL.finditer(code):
        variables.setdefault(m.group("var"), m.start())
    initialized = {}
    for m in CS_NEW_LABEL.finditer(code):
        if not m.group("brace"):
            continue
        body = balanced(code, m.start("brace"))
        dock = CS_DOCKED.search(body)
        auto = CS_AUTOSIZE_TRUE.search(body) is not None
        owner = next((v for v, at in variables.items() if at < m.start() and code[at:m.start()].count(";") == 0), None)
        if owner:
            initialized[owner] = (dock.group("value") if dock else None, auto)
        elif dock and not auto:
            found.append((line_of(code, m.start()), "new Label", "DockStyle." + dock.group("value")))
    for var, at in variables.items():
        dock, auto = initialized.get(var, (None, False))
        later = re.search(r"\b" + re.escape(var) + r"\.Dock\s*=\s*(?:System\.Windows\.Forms\.)?DockStyle\.(?!None\b)(?P<value>\w+)", code)
        if later:
            dock = dock or later.group("value")
        if re.search(r"\b" + re.escape(var) + r"\.AutoSize\s*=\s*true\b", code):
            auto = True
        if dock and not auto:
            found.append((line_of(code, at), var, "DockStyle." + dock))
    return found


def scan_text(path, text):
    """Findings of one file: (line, name, dock value), PowerShell and C# alike."""
    found = []
    if path.suffix.lower() in (".ps1", ".psm1"):
        if not re.search(r"Windows\.Forms", text, re.I):
            return None
        found += scan_powershell(text)
        # C# that a script compiles with Add-Type.
        found += scan_csharp(text)
    else:
        if not re.search(r"\bLabel\b", text):
            return []
        found += scan_csharp(text)
    return sorted(set(found))


def files(root):
    for top, dirs, names in os.walk(root):
        rel = Path(top).relative_to(root)
        dirs[:] = sorted(d for d in dirs if not (rel == Path(".") and d in SKIP_DIRS) and d != ".git")
        for name in sorted(names):
            if Path(name).suffix.lower() in SUFFIXES:
                yield Path(top) / name


def check(root, allowed=None):
    """(problems, report lines, the WinForms files read). A problem is a finding outside allowed, or an allowed
    finding the scan no longer makes."""
    allowed = ALLOWED if allowed is None else allowed
    root = Path(root)
    problems, report, winforms = [], [], []
    seen = set()
    for path in files(root):
        rel = path.relative_to(root).as_posix()
        try:
            text = path.read_text(encoding="utf-8-sig", errors="replace")
        except OSError as e:
            problems.append("%s: cannot read (%s)" % (rel, e))
            continue
        found = scan_text(path, text)
        if found is None:
            continue
        if path.suffix.lower() in (".ps1", ".psm1") or re.search(r"Windows\.Forms", text):
            winforms.append(rel)
        for line, name, dock in found:
            key = (rel, name)
            if key in allowed:
                seen.add(key)
                report.append("%s:%d: %s (Dock %s, AutoSize off) allowed: %s" % (rel, line, name, dock, allowed[key]))
            else:
                problems.append("%s:%d: %s is a Label with Dock %s and AutoSize off (fact M898): at each change of its "
                                "text it becomes 100x23 before its parent docks it again; anchor it or turn AutoSize on"
                                % (rel, line, name, dock))
    for key in sorted(set(allowed) - seen):
        problems.append("%s: the allowed finding %s is not there any more; remove it from ALLOWED" % key)
    return problems, report, winforms


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", required=True, help="the repository")
    args = ap.parse_args(argv)
    problems, report, winforms = check(args.root)
    for line in report:
        print(line)
    for line in problems:
        print("FAIL " + line)
    print("winforms-labels: %d WinForms files read, %d allowed findings, %d problems"
          % (len(winforms), len(report), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
