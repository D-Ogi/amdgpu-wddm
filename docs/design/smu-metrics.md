# SMU metrics validation

The KMD checks each metrics sample before it replaces the last accepted table.
The parser is `driver/shim/bc250_smu_metrics.c`.
Its caller, `driver/kmd/smu_metrics.c`, publishes the result to the fan and the DPM escape.
The clock governor uses separate clock and temperature reads. It does not choose clock levels from this table.

## Layout and reported overwrite

PROVENANCE: Linux v6.18, revision `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`, AMD metrics interface header, MIT.
The contract is [`smu11_driver_if_cyan_skillfish.h`](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_cyan_skillfish.h), interface version `0x8`.

Each half is 116 bytes. `Current` starts at byte 0 and `Average` at byte 116.
Three 32-bit counters follow them. The complete table is 244 bytes.
Offsets below are relative to one half.

The community reports extra stores from cores 6 and 7 on unlocked boards with stock SMU firmware.
The following map shows where those stores would land with the stock array bases and element widths.
It describes the reported mechanism. No eight-core table was captured on our six-core lab board.

| Array | Offset | Element width | Extra stores at | Fields overwritten |
|---|---:|---:|---|---|
| `CoreFrequency[6]` | 0 | 2 bytes | 12, 14 | `CorePower[0]` |
| `CorePower[6]` | 12 | 4 bytes | 36, 40 | `CoreTemperature[0..3]` |
| `CoreTemperature[6]` | 36 | 2 bytes | 48, 50 | `L3Frequency[0..1]` |
| `C0Residency[6]` | 56 | 2 bytes | 68, 70 | `GfxclkFrequency`, `GfxTemperature` |

The KMD publishes the fields below. All follow at least one core array.
The two extra stores shown above directly reach only the GPU clock and temperature among these fields.
They do not directly reach socket power. Broader corruption of a modified table remains possible.

| Field | Offset in `Current` | Unit | Consumer |
|---|---:|---|---|
| `GfxclkFrequency` | 68 | MHz | Telemetry |
| `GfxTemperature` | 70 | centi-Celsius | Telemetry |
| `Voltage[0..1]` | 80, 84 | mV | Telemetry |
| `Power[0..1]` | 96, 100 | mW | Telemetry |
| `CurrentSocketPower` | 104 | mW | Fan load boost and telemetry |
| `SocTemperature` | 108 | centi-Celsius | Telemetry |
| `ThrottlerStatus` | 112 | Opaque 16-bit flags | Telemetry |

The KMD also publishes `Average.CurrentSocketPower` at absolute offset 220.
Rail 0 is `VDDCR_VDD`, the CPU rail. The public ABI calls its fields `SocMv` and `SocPowerMw` for compatibility.
Rail 1 is `VDDCR_GFX`.

## Admission bounds

These are parser bounds, not permitted hardware settings.
[Hardware limits](../hardware.md) and the [DPM table](dpm.md) still constrain every clock or voltage request.

| Value | Accepted range | Basis |
|---|---|---|
| Each power field | 0 to 300000 mW | The board supply is 300 W |
| Current and average socket power | Nonzero within that range | Zero samples already have a separate skip rule |
| GPU voltage | 0 to 1175 mV | 1075 mV overclock-tier ceiling plus 100 mV readback allowance |
| CPU rail voltage | 0 to 1600 mV | Existing CPU voltage readback envelope |
| GPU clock | 0, or 500 to 2415 MHz | Idle point through the 2300 MHz overclock-tier ceiling plus 5 percent readback allowance; a lower bound stops the reader at a tier clock |
| Each published temperature | 0 to 15000 centi-Celsius | Existing broad parser envelope |
| Throttler flags | 0 to 65534 | Preserve opaque bits, reject the complete poison word |

The clock and voltage allowances are conservative software choices. They are not measured sensor error bounds.
Zero clock or voltage can describe a gated rail. A temperature above the thermal trip must remain visible.
No check treats 87 C or 89 C as malformed telemetry.
Every complete poison field, including rail power and throttler flags, rejects the sample.

The parser also checks the GPU clock and temperature together.
If both are at most 100 in their raw units, they resemble two percentage values from `C0Residency[6..7]`.
The sample is rejected with reason 5 before the ordinary clock check.
This pattern is a diagnostic signature, not proof of an eight-core firmware variant.
Values corrupted into otherwise plausible ranges can evade these checks. The parser does not authenticate firmware data.

## Refusals, expiry and diagnostics

A refused table never replaces the last accepted sample or refreshes its timestamp.
The existing three-second age limit stays in force. Three bad tables stop the reader for that device start.
Transport refusal stops reads for the lifetime of the loaded driver image.
Zero socket power keeps its existing skip rule and does not count toward the bad-table limit.

Without fresh metrics, `SmuMetricsFill` returns false and the DPM escape clears `BC250_DPM_FLAG_POWER`.
The fan receives `PowerValid = false` and zero socket power.
Its temperature curve, emergency rule, temperature-rise detection and GPU-load boost continue to operate.

ABI 3 stays 248 bytes. Its existing `MetricsFailures` counts unsuccessful reads, including rejected tables.
There is no reserved word in the metrics tail.
Detailed diagnostics therefore remain in the log, without changing any ABI field:

| Log label | Meaning |
|---|---|
| `reject` | Saturating count of rejected tables in this start |
| `why` | Last rejection reason, retained after a good sample |
| `m67` | Bit mask of spare bytes containing `0x67` in the latest parsed transfer |
| `sp` | Four raw spare bytes, packed from least to most significant in offset order |

Reasons are 0 none, 1 short, 2 unwritten, 3 range, 4 zero power, 5 core-overwrite pattern and 6 clock range.
The mask bits 0 through 3 correspond to absolute offsets 114, 115, 230 and 231.
The community reports `0x67` in padding, but has not specified its exact byte or the second table's layout here.
These four bytes are the named `Spare` members. A zero mask means no marker observed there.
The marker does not select a decoder, bypass validation or authorize access to a second table.
An unknown ACPI patch receives the same checks.

The normal telemetry still uses two log lines. Power values in the first line use integer mW to make space for diagnostics.
The power labels `p`, `a`, `g` and `s` mean socket, average socket, GPU rail and CPU rail.

## Host validation

`driver/shim/test/run_smu_metrics.ps1` compiles the real parser and KMD binding with warnings as errors.
It checks the stock layout, each published field, the extra-core stores, marker handling, refusal history and freshness.
`driver/shim/test/run_hwmon.ps1` checks fan control with missing power and valid temperature or GPU load.
These host tests do not establish which firmware variant a field board runs.
