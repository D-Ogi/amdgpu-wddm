"""Minimal streaming reader for dEQP .qpa logs (log format 0.3.x).

Yields one record per case that the log begins:
    {"case", "status", "duration_us", "details", "complete"}
status is the <Result StatusCode> value for a finished case, the qpTestResult name written by
#terminateTestCaseResult (record["terminated"] is then True), or None for a case that began but never
ended (killed or crashed process).
Session info lines are returned separately by read_session().
"""

import re

_RESULT = re.compile(r'<Result StatusCode="([^"]+)">([^<]*)</Result>')
_DURATION = re.compile(r'<Number Name="TestDuration"[^>]*>([0-9.]+)</Number>')


def iter_cases(path):
    case = None
    buf = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#beginTestCaseResult "):
                if case is not None:  # previous case never ended
                    yield _finish(case, buf, None)
                case = line[len("#beginTestCaseResult "):].strip()
                buf = []
            elif line.startswith("#endTestCaseResult"):
                if case is not None:
                    yield _finish(case, buf, "end")
                case, buf = None, []
            elif line.startswith("#terminateTestCaseResult"):
                reason = line[len("#terminateTestCaseResult"):].strip()
                if case is not None:
                    rec = _finish(case, buf, "end")
                    # qpTestLog_terminateCase writes the qpTestResult name (Crash, Timeout, ...)
                    rec["status"] = reason or "Crash"
                    rec["terminated"] = True
                    yield rec
                case, buf = None, []
            elif case is not None:
                buf.append(line)
    if case is not None:
        yield _finish(case, buf, None)


def _finish(case, buf, how):
    text = "".join(buf)
    status = None
    details = ""
    m = None
    for m in _RESULT.finditer(text):
        pass
    if how == "end" and m:
        status, details = m.group(1), m.group(2)
    elif how == "end":
        status = "NoResult"
    d = _DURATION.search(text)
    return {
        "case": case,
        "status": status,
        "details": details.strip(),
        "duration_us": float(d.group(1)) if d else None,
        "complete": how == "end",
        "terminated": False,
    }


def read_session(path):
    info = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#sessionInfo "):
                parts = line[len("#sessionInfo "):].strip().split(" ", 1)
                info[parts[0]] = parts[1].strip('"') if len(parts) > 1 else ""
            elif line.startswith("#beginTestCaseResult"):
                break
    return info
