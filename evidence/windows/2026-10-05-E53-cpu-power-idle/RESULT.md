# E53: the CPU power policy and the idle P-state on unit A under Windows

Date: 2026-10-05, 02:15-02:18Z. Unit A, Windows 11 Pro build 22631 (boot 2026-10-05T01:34:43Z),
kernel driver 0.7.205.1. Desktop idle: no game, no benchmark, no lab trial in flight.

Read-only run. `tools/win/cpupower/probe-cpu-power.ps1` queries the active power scheme and samples
four counters. It writes no setting. `powercfg /setacvalueindex`, `powercfg /setactive` and
`powercfg /energy` were not used (`/energy` runs for 60 s or more and writes a report file).

Command: `python tools/win/target.py ps tools/win/cpupower/probe-cpu-power.ps1 20`

## Files

| File | Content |
|---|---|
| `probe-output.txt` | the probe's own output: identity, active scheme, counter statistics |
| `powercfg-sub-processor.txt` | `powercfg /qh <active scheme> SUB_PROCESSOR`, unedited (749 lines) |

## What it shows

1. **The processor as Windows sees it**: `AMD BC-250`, 6 cores, 12 logical processors, nominal
   3194 MHz. Both WMI fields (`MaxClockSpeed`, `CurrentClockSpeed`) read 3194: WMI reports the
   nominal frequency here, not the live one.
2. **The active scheme is Balanced** (`381b4222-f694-41f0-9685-ff5bb260df2e`).
3. **The processor runs its own P-states.** `PERFAUTONOMOUS` (processor performance autonomous mode)
   is 1 on AC, so Windows states a desired performance window and the hardware picks the frequency
   inside it. The window is `PROCTHROTTLEMIN` 5 % to `PROCTHROTTLEMAX` 100 %, `PERFEPP` (energy
   performance preference) 33 %, `PERFBOOSTMODE` 2 (aggressive) and `PERFBOOSTPOL` 60 %.
   `PROCFREQMAX` is 0, so no frequency ceiling is set.
4. **Idle states are allowed and core parking is off on AC.** `IDLEDISABLE` 0, `IDLESTATEMAX` 0,
   `CPMINCORES` 100 % and `CPMAXCORES` 100 %. The whole set of cores stays unparked on AC, and the DC
   values park down to 10 %. `THROTTLING` (allow throttle states) is 2.
5. **The idle frequency is about 46 % of maximum.** Over 20 samples at 1 s, idle:
   `% of Maximum Frequency` mean 46.00, minimum 42.00, maximum 100.00.
   `% Processor Performance` mean 62.66, minimum 42.98, maximum 106.15.
   `% Processor Time` mean 3.33 %, maximum 25.60 %.
   42 % of 3194 MHz is about 1.34 GHz, so the cores do drop their clock at idle.
   `% Processor Performance` above 100 % is the boost above the nominal 3194 MHz.
6. **The `Processor Frequency` counter is not usable on this part**: it read 3194 in all 20 samples
   while `% of Maximum Frequency` moved between 42 and 100. Use the two percentage counters.

## What it does not show

- The maxima (100.00 and 106.15) come from the samples in which the probe itself ran. This is idle
  behaviour with a 1 s sampler on it, not a quiet-system floor.
- No load point was measured. The under-load behaviour of the same counters is a separate run.
- No power figure. The smart plug was not read in this run, so nothing here prices the CPU in watts.
- The scheme values are the ones in the registry for the Balanced scheme on this image. Nothing was
  changed, so the run does not show whether another scheme would behave differently.
