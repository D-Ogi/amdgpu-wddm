"""Extract the production replug request and its escape wrapper for a CPU-only test."""
import argparse
from pathlib import Path
import re
import sys


def body(source, name):
    found = list(re.finditer(r"(?m)^(?:static )?(?:NTSTATUS|void|LONG|int|BOOLEAN) " + name + r"\([^;]*?\)\s*\{", source))
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
    sys.path.insert(0, str(here.parents[2] / "tools/quality"))
    from escape_commands import check
    commands, errors = check(here.parents[2])
    if errors:
        raise SystemExit("\n".join(errors))
    names = {c["name"] for c in commands}
    checks = (here.parent / "escape_commands.h").read_text(encoding="utf-8")
    covered = set(re.findall(r"case (BC250_\w+):", checks))
    if names != covered:
        raise SystemExit(f"Escape namespace check differs: missing {sorted(names-covered)}, extra {sorted(covered-names)}")
    actual = "\n\n".join(body(connector, name) for name in ("ReadAtomic", "ConnectorDpc", "ConnectorReplug"))
    actual += "\n\n" + body(display, "ConnectorReplugEscape")
    template = (here / "connector_replug_test.c").read_text(encoding="utf-8")
    if template.count("/* ACTUAL_SOURCE */") != 1:
        raise SystemExit("Production source marker changed")
    args.out.write_text(template.replace("/* ACTUAL_SOURCE */", actual), encoding="utf-8")
    # Keep the complete dispatcher, including the earlier HIP branch and release
    # guards. Only hardware/token endpoints are mocked by the host fixture.
    cli = (here.parents[2] / "tools/win/bc250kmd_cli/bc250kmd_cli.c").read_text(encoding="utf-8")
    dispatch = (here / "connector_dispatch_test.c").read_text(encoding="utf-8")
    actual_dispatch = body(display, "WddmDiagnosticAllowed") + "\n\n" + body(display, "Bc250Escape")
    actual_cli = "\n\n".join(body(cli, name) for name in
                             ("EscapeOn", "CloseAdapterHandle", "SendEscapeFlags", "ConnectorReplug"))
    actual_cli = actual_cli.replace("static int ConnectorReplug(void)", "static int CliConnectorReplug(void)")
    for marker, text in (("/* ACTUAL_DISPATCH */", actual_dispatch), ("/* ACTUAL_CLI */", actual_cli)):
        if dispatch.count(marker) != 1:
            raise SystemExit(f"Expected one {marker}")
        dispatch = dispatch.replace(marker, text)
    args.out.with_name("connector_dispatch_actual.c").write_text(dispatch, encoding="utf-8")


if __name__ == "__main__":
    main()
