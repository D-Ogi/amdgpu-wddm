"""Extract the production replug request and its escape wrapper for a CPU-only test."""
import argparse
from pathlib import Path
import re


def body(source, name):
    found = list(re.finditer(r"(?m)^(?:static )?(?:NTSTATUS|void|LONG) " + name + r"\([^;]*?\)\s*\{", source))
    if len(found) != 1:
        raise SystemExit(f"Expected one definition of {name}, found {len(found)}")
    start = found[0].start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    connector = (here.parent / "connector.c").read_text(encoding="utf-8")
    display = (here.parent / "display.c").read_text(encoding="utf-8")
    actual = "\n\n".join(body(connector, name) for name in ("ReadAtomic", "ConnectorDpc", "ConnectorReplug"))
    actual += "\n\n" + body(display, "ConnectorReplugEscape")
    template = (here / "connector_replug_test.c").read_text(encoding="utf-8")
    if template.count("/* ACTUAL_SOURCE */") != 1:
        raise SystemExit("Production source marker changed")
    args.out.write_text(template.replace("/* ACTUAL_SOURCE */", actual), encoding="utf-8")


if __name__ == "__main__":
    main()
