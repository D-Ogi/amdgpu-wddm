"""Compile the production wave-access wrapper against a CPU-only fault model."""
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--display", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    start = "typedef struct GFX_WAVE_CONTEXT {"
    end = "void GfxRecordWavesBeforeKill("
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit("Wave wrapper extraction anchors changed")
    body = source[source.index(start):source.index(end)]
    template = Path(__file__).with_name("wave_access_test.c").read_text(encoding="utf-8")
    marker = "/* ACTUAL_SOURCE */"
    if template.count(marker) != 1:
        raise SystemExit("Test template must have one production-source marker")
    display = args.display.read_text(encoding="utf-8")
    begin_display = "static BOOLEAN WaveCallerIsAdmin(void)"
    end_display = "NTSTATUS Bc250Escape("
    if display.count(begin_display) != 1 or display.count(end_display) != 1:
        raise SystemExit("Wave escape extraction anchors changed")
    escape_body = display[display.index(begin_display):display.index(end_display)]
    escape_marker = "/* ACTUAL_ESCAPE */"
    if template.count(escape_marker) != 1:
        raise SystemExit("Test template must have one escape-source marker")
    args.out.write_text(template.replace(marker, body).replace(escape_marker, escape_body), encoding="utf-8")


if __name__ == "__main__":
    main()
