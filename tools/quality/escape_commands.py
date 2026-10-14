"""Check private escape commands across KMD headers and the shared HIP ABI.

Command definitions use BC250_ESCAPE_* or BC250_HIP_JOURNAL_COMMAND. MAGIC, STATUS_* and FLAG_* describe
other fields of the wire format. All other definitions must be literal DWORDs.
Conditional definitions are checked together so build variants cannot collide.
"""
import argparse
import json
from pathlib import Path
import re


DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(BC250_ESCAPE_\w+|BC250_HIP_JOURNAL_COMMAND)\b(.*)$", re.MULTILINE)
LITERAL = re.compile(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)(?:[uU](?:[lL]{1,2})?|[lL]{1,2}[uU]?)?$")


def check_sources(sources):
    commands, errors, numbers, names = [], [], {}, {}
    for path, source in sorted(sources.items()):
        source = re.sub(r"\\\r?\n", "", source)
        source = re.sub(r"/\*.*?\*/", lambda m: " " + "\n" * m[0].count("\n"), source, flags=re.S)
        source = re.sub(r"//[^\n]*", "", source)
        for match in DEFINE.finditer(source):
            name, expression = match.groups()
            if name == "BC250_ESCAPE_MAGIC" or name.startswith(("BC250_ESCAPE_STATUS_", "BC250_ESCAPE_FLAG_")):
                continue
            where = f"{path}:{source.count(chr(10), 0, match.start()) + 1}"
            expression = expression.strip()
            while expression.startswith("(") and expression.endswith(")"):
                expression = expression[1:-1].strip()
            if not LITERAL.fullmatch(expression):
                errors.append(f"{where}: {name} must define a literal command number, got {expression!r}")
                continue
            literal = re.sub(r"[uUlL]+$", "", expression)
            # Leading zero decimal constants are not accepted: C would interpret them as octal.
            if len(literal) > 1 and literal[0] == "0" and literal[1] not in "xX":
                errors.append(f"{where}: {name} has an ambiguous leading zero")
                continue
            number = int(literal, 16 if literal.lower().startswith("0x") else 10)
            if not 1 <= number <= 0xFFFFFFFF:
                errors.append(f"{where}: {name} is outside the nonzero DWORD command range")
                continue
            if name in names:
                errors.append(f"{where}: duplicate command name {name}; first at {names[name]}")
            if number in numbers:
                errors.append(f"{where}: duplicate command number {number}: {name}; first {numbers[number]}")
            names[name] = where
            numbers[number] = f"{name} at {where}"
            commands.append({"name": name, "number": number, "source": where})
    if not commands:
        errors.append("no escape command definitions found")
    return commands, errors


def check(root):
    headers = sorted(p for p in (root / "driver" / "kmd").rglob("*") if p.is_file() and p.suffix.lower() == ".h")
    shared = root / "driver/shim/include/bc250_hip_journal.h"
    if not shared.is_file():
        return [], ["missing shared escape ABI: driver/shim/include/bc250_hip_journal.h"]
    headers.append(shared)
    return check_sources({p.relative_to(root).as_posix(): p.read_text(encoding="utf-8-sig") for p in headers})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    commands, errors = check(args.root)
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "escape-commands.json").write_text(json.dumps({"commands": commands, "errors": errors}, indent=2) + "\n", encoding="utf-8")
    for error in errors:
        print(error)
    print(f"escape commands: {len(commands)} definitions, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
