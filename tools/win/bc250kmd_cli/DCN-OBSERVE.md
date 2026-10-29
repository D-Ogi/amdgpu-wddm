# DCN observation control

This tool reads the existing OBSERVE_DCN command on the release or lab KMD.
It does not change the driver, mode, clocks, registers, allocations or capabilities.
HardwareAccess temporarily idles GPU scheduling. Run this control outside performance measurements.
The tool selects exactly one AMD device 13FE through DXGI and its adapter LUID.
It refuses a different KMD version before register reads.

## Build

1. Build with explicit clang-cl 23.1.4:

   ```powershell
   tools/win/bc250kmd_cli/build-dcn-observe.ps1 -Root <workspace> -Out <build-directory>
   ```

2. Run the offline evidence tests:

   ```powershell
   python -B -m unittest discover -s tools/win/bc250kmd_cli -p test_dcn_observe_client.py -v
   ```

3. Before promotion, run the runner controls from an elevated shell:

   ```powershell
   tools/win/bc250kmd_cli/test-dcn-observe-runner.ps1 -Stub <build-directory>/dcn_observe_runner_stub.exe -Out <build-directory>
   ```

   These controls use Windows PowerShell 5.1 and the real runner with a host stub.
   They check process exit codes 0 and 1, retained output and caller success/failure.
   They do not open an adapter. The build can run them with `-RunRunnerControls`.

## Lab procedure

Keep the whole trial within three minutes. The capture has a separate 30-second watchdog.
Do not run this tool on the development PC's display adapter.

1. Obtain the lab slot from the release owner.
2. Record the loaded KMD identity and current boot through the standard lab preflight.
3. Record the display mode and whether the monitor has a visible desktop.
4. Retain a half-resolution lab screenshot.
5. Copy the executable and runner through `tools/win/target.py`.
6. Use an elevated lab shell to run the capture:

   ```powershell
   .\run-dcn-observe.ps1 -Exe <executable> -ExpectedSha256 <build-hash> -ExpectedVersion 0x000700D8 -Out <new-directory>
   ```

7. Retain `samples.jsonl`, `errors.jsonl` and `run.json` without edits.
8. Check the lab's desktop and device health after capture.
9. Copy the evidence through `tools/win/target.py`.
10. Analyze the retained samples locally:

    ```powershell
    python tools/win/bc250kmd_cli/analyze-dcn-observe.py <samples.jsonl>
    ```

The example version is the current KMD protocol word. Match it to the preflight output.
That word does not identify a build or replace the loaded-image hash.
The default capture reads 20 samples, with 250 ms between calls.
No test stimulus starts automatically. An idle desktop can keep the same surface throughout.

If the watchdog fires, retain the evidence and inspect lab health through the existing channels.
Do not repeat the capture automatically. Process termination does not establish GPU recovery.

## Interpret the observation

The hypothesis is that the existing release reader provides complete, useful HUBP0 and OTG0 observations on the current candidate.
Complete replies and an advancing frame counter support that limited hypothesis.
A refusal, partial reply or fixed counter requires investigation before using this channel for further experiments.
Check the timing control and display state before attributing a fixed counter to a hardware failure.

| Observation | Supported conclusion | Remaining question |
|---|---|---|
| Complete replies, frame count changes | The reader observed OTG frame changes | Visible content and plane latch |
| Software sequence changes or is odd | Software publication overlaps the read | Which register values form one state |
| Clear pending bit, unequal address words | A candidate mismatch needs a coherent capture | Latch order or a torn read |
| Equal primary and earliest address words | Those sequential words match | Attribute completion and retirement |
| No address changes | This capture contains no address transition | Behavior during a flip |
| Slow escape calls | The observation itself consumed time | Unperturbed hardware latency |

The old SURFACE_INUSE observation in fact M89 read zero while addresses changed.
This reader uses SURFACE_EARLIEST_INUSE instead. Do not interchange these registers.
It reads low then high once, without the production reader's high-low-high retry.
An unchanged software sequence cannot exclude a hardware latch between those reads.

This control cannot test HUBP1, CSC bank selection, coefficient retirement or atomic two-plane updates.
It cannot establish the last memory read from an old allocation.
The analyzer always leaves latch and retirement unproven, including when all samples match.
Before an MPO hardware trial, add generation-bound observations to the actual programming and completion path.
Include same-address attribute changes and plane disable as negative controls.
Keep both allocation sets referenced until the complete retirement predicate is established.
Do not use this capture as an MPO or HLK pass.
