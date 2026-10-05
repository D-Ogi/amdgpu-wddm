#!/usr/bin/env python3
"""Offline checks of the plug tool. Touches no network and no real device.

    python tools/win/smartplug/selftest.py

The checks cover what the tool decides without the device: configuration loading, the measurement
scales, the relay readback rule and the refusal of an answer that carries no relay state. A stub
device stands in for the plug, so this runs on any machine, with or without the configuration file.
"""

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import plug  # noqa: E402

# Invented values. The identifier, the key and the address are documentation placeholders
# (RFC 5737 TEST-NET-1), never a real device.
SAMPLE_CONFIG = {"id": "deviceid", "key": "0123456789abcdef", "ip": "192.0.2.10", "version": "3.4"}
SAMPLE_DPS = {"1": True, "18": 831, "19": 1102, "20": 2428, "38": "off", "40": "relay"}

failures = []


def check(name, fn):
    try:
        fn()
    except Exception as e:  # a failed check is a result, not a crash
        failures.append(f"{name}: {type(e).__name__}: {e}")
        print(f"FAIL {name}: {type(e).__name__}: {e}")
    else:
        print(f"ok   {name}")


class StubDevice:
    """A plug that answers from a dictionary. `accepts` false makes it ignore every write."""

    def __init__(self, dps=None, accepts=True, answer=None):
        self.dps = dict(SAMPLE_DPS if dps is None else dps)
        self.accepts = accepts
        self.answer = answer
        self.refreshed = []
        self.closed = False

    def status(self):
        return self.answer if self.answer is not None else {"dps": dict(self.dps)}

    def set_status(self, on):
        if self.accepts:
            self.dps["1"] = on

    def updatedps(self, points, nowait=False):
        self.refreshed.append(list(points))

    def close(self):
        self.closed = True


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_error(fn, fragment):
    try:
        fn()
    except plug.PlugError as e:
        expect(fragment in str(e), f"wrong message: {e}")
        return
    raise AssertionError(f"expected a PlugError naming {fragment!r}")


def t_config_ok():
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "plug-local.json"
        path.write_text(json.dumps(SAMPLE_CONFIG), encoding="utf-8")
        cfg = plug.load_config(str(path))
        expect(cfg["id"] == SAMPLE_CONFIG["id"], "identity lost")
        expect(cfg["version"] == "3.4", "version lost")


def t_config_missing_file():
    with tempfile.TemporaryDirectory() as d:
        expect_error(lambda: plug.load_config(str(Path(d) / "absent.json")), "no plug configuration")


def t_config_incomplete():
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "plug-local.json"
        path.write_text(json.dumps({"id": "deviceid", "ip": "192.0.2.10"}), encoding="utf-8")
        expect_error(lambda: plug.load_config(str(path)), "leaves out key, version")


def t_config_not_json():
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "plug-local.json"
        path.write_text("not json", encoding="utf-8")
        expect_error(lambda: plug.load_config(str(path)), "not valid JSON")


def t_state_reads_relay():
    expect(plug.state(StubDevice({"1": True})) is True, "closed relay not read")
    expect(plug.state(StubDevice({"1": False})) is False, "open relay not read")


def t_state_refuses_no_relay():
    expect_error(lambda: plug.state(StubDevice({"18": 831})), "no valid relay status")
    expect_error(lambda: plug.state(StubDevice(answer="Error")), "no valid relay status")
    expect_error(lambda: plug.state(StubDevice({"1": 1})), "no valid relay status")


def t_set_state_readback():
    device = StubDevice({"1": False})
    expect(plug.set_state(device, True, settle=0) is True, "write not confirmed")
    expect(plug.set_state(device, False, settle=0) is False, "write not confirmed")


def t_set_state_refuses_mismatch():
    device = StubDevice({"1": False}, accepts=False)
    expect_error(lambda: plug.set_state(device, True, settle=0), "readback mismatch")


def t_telemetry_scales():
    reading = plug.decode_telemetry(SAMPLE_DPS, queried_utc="2026-09-24T02:47:14+00:00")
    expect(reading["current_a"] == 0.831, f"current {reading['current_a']}")
    expect(reading["power_w"] == 110.2, f"power {reading['power_w']}")
    expect(reading["voltage_v"] == 242.8, f"voltage {reading['voltage_v']}")
    expect(reading["raw_dps"] == {"18": 831, "19": 1102, "20": 2428}, "raw values lost")
    expect(reading["measurement_age_seconds"] is None, "sample age is not known")
    expect(reading["queried_utc"] == "2026-09-24T02:47:14+00:00", "query time lost")
    expect(reading["relay_on"] is True, "relay state lost")


def t_telemetry_refuses_partial():
    expect_error(lambda: plug.decode_telemetry({"1": True, "18": 831, "19": 1102}), "measurement fields")
    expect_error(lambda: plug.decode_telemetry({"18": True, "19": 1102, "20": 2428}), "measurement fields")


def t_telemetry_asks_for_a_refresh():
    device = StubDevice()
    reading = plug.telemetry(device, settle=0)
    expect(device.refreshed == [[18, 19, 20]], f"refresh request {device.refreshed}")
    expect(reading["transport"] == "LAN", "transport lost")
    expect(device.dps["1"] is True, "telemetry moved the relay")


def t_module_import_reads_nothing():
    expect("tinytuya" not in sys.modules, "importing the tool pulled in the protocol library")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("t_"):
            check(name[2:].replace("_", " "), fn)
    print(f"{'FAILED' if failures else 'passed'}: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
