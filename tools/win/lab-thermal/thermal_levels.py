"""The temperature levels of the lab tools, read from thermal-levels.json beside this file.

    python tools/win/lab-thermal/thermal_levels.py            the levels, one per line
    python tools/win/lab-thermal/thermal_levels.py --shell    shell assignments (STOP_HELD_C=92 ...) for a bash tool

A host tool imports this module; a lab script reads the JSON file that its stager puts beside it. Both read the same
bytes, and no tool holds a number of its own (test_thermal_levels.py is the gate). README.md gives the contract.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, fields
from pathlib import Path

PATH = Path(__file__).resolve().parent / "thermal-levels.json"
SCHEMA = "bc250.lab-thermal-levels.v1"


@dataclass(frozen=True)
class Levels:
    """Tctl levels in degrees Celsius, and the hold time in seconds."""

    driver_throttle_c: float   # the kernel driver lowers the clock from here; a tool never stops a run for it
    start_below_c: float       # no run starts at or above this temperature
    measured_start_below_c: float  # a measured run (game session, train arm) starts only below this line
    stop_held_c: float         # a run stops when every reading for stop_held_s is at or above this temperature
    stop_held_s: float
    stop_at_once_c: float      # a run stops at the first reading at or above this temperature

    def describe(self) -> str:
        return (f"Tctl {number(self.stop_held_c)} C held {number(self.stop_held_s)} s, "
                f"or {number(self.stop_at_once_c)} C at once")


def number(value: float) -> str:
    """92.0 as '92', 92.5 as '92.5': the form the documents and the lab lines use."""
    return str(int(value)) if float(value).is_integer() else str(value)


def check(data: dict) -> Levels:
    if data.get("schema") != SCHEMA:
        raise ValueError(f"thermal levels: schema is {data.get('schema')!r}, not {SCHEMA}")
    values = {}
    for item in fields(Levels):
        value = data.get(item.name)
        if type(value) not in (int, float):
            raise ValueError(f"thermal levels: {item.name} must be a number")
        values[item.name] = float(value)
    levels = Levels(**values)
    if not 0 < levels.measured_start_below_c <= levels.start_below_c < levels.stop_held_c < levels.stop_at_once_c:
        raise ValueError("thermal levels: the order must be 0 < measured_start_below_c <= start_below_c < "
                         "stop_held_c < stop_at_once_c")
    if not levels.driver_throttle_c <= levels.stop_held_c:
        raise ValueError("thermal levels: a run must not stop below the driver throttle")
    if levels.stop_held_s <= 0:
        raise ValueError("thermal levels: stop_held_s must be above 0")
    return levels


def load(path: Path | None = None) -> Levels:
    return check(json.loads(Path(path or PATH).read_text(encoding="utf-8")))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--shell", action="store_true", help="print NAME=value lines for a shell")
    args = parser.parse_args(argv)
    levels = load()
    if args.shell:
        # A bash on Windows reads "92\r" from a CRLF line, and awk then reads no number.
        sys.stdout.reconfigure(newline="\n")
    for item in fields(Levels):
        value = number(getattr(levels, item.name))
        print(f"{item.name.upper()}={value}" if args.shell else f"{item.name} {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
