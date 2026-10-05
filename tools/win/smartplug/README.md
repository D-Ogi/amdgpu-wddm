# smartplug - mains power for the lab unit, over the local network

The lab unit hangs. Then ssh does not answer, the emergency channel does not answer, and the board has
no BMC and no IPMI. A smart plug in the mains lead is the only remaining reset. This tool operates that
plug from the development PC over the local network, with no cloud call, and reads the relay back after
every write. The device confirms every command that reports success.

```
python tools/win/smartplug/plug.py status       read the relay, change nothing
python tools/win/smartplug/plug.py on           close the relay, then read it back
python tools/win/smartplug/plug.py off          open the relay, then read it back
python tools/win/smartplug/plug.py telemetry    current, power and voltage, with the raw values
python tools/win/smartplug/selftest.py          offline checks; no network, no device
```

Each command prints one JSON object on stdout.

```json
{"transport": "LAN", "relay_on": true, "readback_verified": true}
```

Exit code 0 means that the device confirmed the result. Exit code 1 means that nothing confirmed the
state: read the relay again before you act on it. The failure line on stderr names the error type and
never the configuration content.

PROVENANCE: the LAN protocol comes from TinyTuya (MIT), kept as a portable package in
`<BC250_ROOT>/toolchain/smartplug-python`.

## Configuration

The device identity and the credentials are **not** in this repository. They come from a JSON file
outside it, by default `<BC250_ROOT>/secrets/smartplug/plug-local.json`. `BC250_SMARTPLUG_CONFIG` or
`--config <path>` points elsewhere. `BC250_ROOT` is the workspace root, by default the parent directory
of this repository.

```json
{
  "id": "<the device identifier>",
  "key": "<the device local key>",
  "ip": "<the plug address on the local network>",
  "version": "3.4"
}
```

The tool needs all four fields. Never print the file and never copy it into this repository: the key is a
credential, and it operates the mains supply of the lab. Importing `plug.py` reads nothing. Only
connecting does, so the self-test and any script that only imports the module work without the file.

`BC250_TINYTUYA` points at the protocol package when it is not in the default place.

## The operator copy

A second, older copy of this tool lives in the workspace at `<BC250_ROOT>\scratch\smartplug\plug.py`. It is
the copy that the workspace recovery instructions name, and committed scripts call it by that path:
`experiments/E32-m11-robustness/stuck_observe.py` and the power-cycle scripts under
`evidence/windows/2026-09-25-E27-m9-recovery/`. That copy keeps the workspace root in its source, has no
`--config` and no `BC250_SMARTPLUG_CONFIG`, and prints a slightly different JSON shape.

This directory holds the source of record. Change the file here first. Copy the change to the operator
directory afterwards, and only after a deliberate check, because that copy is the one an operator runs
while the lab hangs. A fix that stays here never reaches the AC cycle that a recovery actually performs.

## Two rules the lab depends on

**Power alone never proves that the operating system responds.** A plug that reports watts proves that
the board draws current, nothing more. A hung machine draws current. A machine in a boot loop draws
current. Ask ssh, or the emergency channel, before you call the lab alive, and never let a power
reading start an automatic power cycle.

**Hard AC cycling is for a hung or unreachable unit.** Read the relay and try ssh first. Prefer a normal
shutdown when the operating system answers. Never cycle the mains routinely between experiments, and
never repeat a cycle quickly: the board boots on its own after AC returns, which is a board jumper and
not a setting anything here can change.

## The measurements

`telemetry` asks the device to refresh its measurement data points, waits, and reads them. The relay
does not move.

| Field | What it is |
|---|---|
| `raw_dps` | The values as the device gave them, data points 18, 19 and 20. Keep these next to any result |
| `current_a`, `power_w`, `voltage_v` | The same values scaled by the convention of the protocol library |
| `queried_utc` | When this tool asked. It is not when the device sampled |
| `measurement_age_seconds` | Always `null`. The device does not report the age of a sample |
| `scale_source` | The origin of the scales, recorded with every reading |

The scale caveat, in full: the divisors (1000 for milliamperes, 10 for tenths of a watt and tenths of a
volt) are the protocol library's socket convention. They are **not calibrated against an external meter
for this model**. The absolute watt figure can therefore be wrong by a constant factor. Comparisons
between two runs of the same workload on the same plug are still useful, because the error is the same
in both. A stale value is possible as well: one reading of zero changed only after a refresh, so the
sample age is unknown and `measurement_age_seconds` stays `null` instead of pretending otherwise.

Whole-unit wall power supports matched comparisons between operating systems and between clock points.
Sample the plug at the start and the end of each stage of a power experiment, and record the raw values
and the query time next to the stage.

## Scope

The tool does four things: read the relay, close it, open it, and read the measurements. It does no
provisioning, no pairing and no firmware work.

Do not reset or re-pair the plug. That can change its local key and leave this tool without a way in.
The one-time credential retrieval that produced the key is not part of this tool and is not run again.

Local network control does not make the device independent of the vendor's servers. Nothing here blocks
the device's own network access, and no test covers its behaviour with no internet.
