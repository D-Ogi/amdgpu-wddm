#!/usr/bin/env python3
"""Local-only control and telemetry of the identified lab power plug. No cloud calls.

The plug carries unit A's mains supply. It is the out-of-band recovery instrument for a hung or
unreachable lab: the board has no BMC and no IPMI, so AC off and on is the only reset that always
works. This tool speaks the plug's LAN protocol directly and reads the relay back after every write,
so a command that reports success has been confirmed by the device.

    python tools/win/smartplug/plug.py status       read the relay (no change)
    python tools/win/smartplug/plug.py on           close the relay, then read it back
    python tools/win/smartplug/plug.py off          open the relay, then read it back
    python tools/win/smartplug/plug.py telemetry    current, power and voltage, with the raw values

Each command prints one JSON object on stdout. Exit code 0 means the device confirmed the result.
Exit code 1 means the state is unconfirmed: read the relay again before you act on it.

The device identity and the credentials are not in this repository. They come from a JSON file
outside it, by default `<BC250_ROOT>/secrets/smartplug/plug-local.json`. `--config <path>` and the
environment variable `BC250_SMARTPLUG_CONFIG` point somewhere else. Importing this module never
reads that file. Only connecting does.

Two rules that the lab depends on:

* Power alone never proves that the operating system responds. A plug that reports watts proves that
  the board draws current, nothing more. Ask ssh, or the emergency channel, before you call the lab
  alive, and never let a power reading start an automatic power cycle.
* The measurements are not calibrated for this model, and the query time is not the sample time.
  `tools/win/smartplug/README.md` has the scale caveat in full.

Do not reset or re-pair the plug. That can change its local key and leave this tool without a way in.
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# BC250_ROOT is the workspace root; by default the parent directory of this repository
# (this file is tools/win/smartplug/plug.py, so the repository root is three levels up from here).
ROOT = os.environ.get("BC250_ROOT", str(Path(__file__).resolve().parents[3].parent))
CONFIG = os.environ.get("BC250_SMARTPLUG_CONFIG", os.path.join(ROOT, "secrets", "smartplug", "plug-local.json"))
# The LAN protocol library, as a portable package outside this repository.
TINYTUYA = os.environ.get("BC250_TINYTUYA", os.path.join(ROOT, "toolchain", "smartplug-python"))

# The device reports its measurements on these data points.
DPS_CURRENT, DPS_POWER, DPS_VOLTAGE = "18", "19", "20"
DPS_RELAY = "1"
SCALE_SOURCE = "TinyTuya SocketDevice convention; model calibration unverified"


class PlugError(RuntimeError):
    """A plug command that did not finish, or finished without a readback."""


def load_config(path=None):
    """The plug identity and credentials. Never print the result: it holds the local key."""
    path = path or CONFIG
    try:
        with open(path, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except FileNotFoundError:
        raise PlugError(f"no plug configuration at {path}; see tools/win/smartplug/README.md for its shape")
    except ValueError as e:
        raise PlugError(f"{path} is not valid JSON: {e}")
    missing = [field for field in ("id", "key", "ip", "version") if not cfg.get(field)]
    if missing:
        raise PlugError(f"{path} leaves out {', '.join(missing)}")
    return cfg


def _library(path=None):
    """The LAN protocol library. Imported here, so that importing this module needs nothing."""
    path = path or TINYTUYA
    if path and os.path.isdir(path) and path not in sys.path:
        sys.path.insert(0, path)
    try:
        import tinytuya
    except ImportError:
        raise PlugError(f"no tinytuya package; expected a portable copy at {path}")
    return tinytuya


def connect(path=None, timeout=5, retries=1):
    """A device handle on the local network. Close it when the command is done."""
    cfg = load_config(path)
    tinytuya = _library()
    device = tinytuya.OutletDevice(cfg["id"], cfg["ip"], cfg["key"], version=float(cfg["version"]))
    device.set_socketTimeout(timeout)
    device.set_socketRetryLimit(retries)
    return device


def state(device):
    """The relay, read from the device. True is closed, so the lab has mains."""
    answer = device.status()
    relay = answer.get("dps", {}).get(DPS_RELAY) if isinstance(answer, dict) else None
    if not isinstance(relay, bool):
        raise PlugError("no valid relay status received over LAN")
    return relay


def set_state(device, on, settle=1.0):
    """Write the relay and read it back. Raises if the device did not take the write."""
    device.set_status(on)
    if settle:
        time.sleep(settle)
    actual = state(device)
    if actual != on:
        raise PlugError("relay state readback mismatch")
    return actual


def decode_telemetry(raw, queried_utc=None):
    """The measurement data points as amperes, watts and volts, with the raw values kept.

    The scales come from the bundled library's socket convention. They are not calibrated against an
    external meter for this model, and the device does not say how old a sample is, so the query time
    is recorded and the sample age stays unknown.
    """
    fields = (DPS_CURRENT, DPS_POWER, DPS_VOLTAGE)
    if not all(isinstance(raw.get(k), int) and not isinstance(raw.get(k), bool) for k in fields):
        raise PlugError("measurement fields unavailable")
    return {
        "queried_utc": queried_utc or datetime.now(timezone.utc).isoformat(),
        "transport": "LAN",
        "relay_on": raw.get(DPS_RELAY),
        "raw_dps": {k: raw[k] for k in fields},
        "current_a": raw[DPS_CURRENT] / 1000,
        "power_w": raw[DPS_POWER] / 10,
        "voltage_v": raw[DPS_VOLTAGE] / 10,
        "scale_source": SCALE_SOURCE,
        "measurement_age_seconds": None,
    }


def telemetry(device, settle=2.0):
    """Ask the device to refresh its measurements, then read them. The relay does not move."""
    device.updatedps([int(DPS_CURRENT), int(DPS_POWER), int(DPS_VOLTAGE)], nowait=True)
    if settle:
        time.sleep(settle)
    answer = device.status()
    raw = answer.get("dps", {}) if isinstance(answer, dict) else {}
    return decode_telemetry(raw)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Local-only control and telemetry of the lab power plug.")
    parser.add_argument("command", choices=["status", "on", "off", "telemetry"])
    parser.add_argument("--config", help="the plug configuration file (default: BC250_SMARTPLUG_CONFIG, "
                                         "then <BC250_ROOT>/secrets/smartplug/plug-local.json)")
    args = parser.parse_args(argv)
    device = connect(args.config)
    try:
        if args.command == "telemetry":
            print(json.dumps(telemetry(device)))
        else:
            on = state(device) if args.command == "status" else set_state(device, args.command == "on")
            print(json.dumps({"transport": "LAN", "relay_on": on, "readback_verified": True}))
    finally:
        device.close()
    return 0


def cli(argv=None):
    """main() with the operator-facing failure line. Says nothing about the configuration content."""
    try:
        return main(argv)
    except SystemExit:
        raise
    except Exception as e:
        print(f"Local plug command failed; state is unconfirmed: {type(e).__name__}", file=sys.stderr)
        if isinstance(e, PlugError):
            print(str(e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(cli())
