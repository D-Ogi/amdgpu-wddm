# Same-process retained GPU probe for S4

Source/build only. No lab access, hibernation or GPU execution on the development PC.

## Artifact

- gpu-residency-probe.exe in this directory
- SHA256 E9B690115E841AB7107D8D16219D320AD70D71595D7F9A9ED1F704150EE56D3A
- Native /W4 /WX /O2 /MT build and --help pass; build.log.

## Run on the lab through the existing persistent process runner

Use unique ready/release and output paths for each trial. Do not restart the probe
at D0: the original PID must survive the actual S4 transition.

    gpu-residency-probe.exe 67108864 vram --resume-gate C:\BC250\tmp\s4-146.ready C:\BC250\tmp\s4-146.release 900000

The paths are positional arguments after --resume-gate; quote each if it contains
spaces. Timeout is 1..1800000 milliseconds. Default no-gate eviction behavior and
--resident-only are retained; --resident-only and --resume-gate are incompatible.

1. Capture stdout/stderr and native exit status using the established background
   runner. The probe creates one data allocation, command allocation, readback
   allocation, context and monitored fence, fills the source once, then verifies
   every source word with real GPU DMA readback.
2. Wait for RESUME_GATE READY and complete ready-file contents. The ready file
   is CREATE_NEW and flushed. It contains pid=<decimal>, first_full_readback=PASS,
   sequence=64 for this 64 MiB test, and timeout_ms=900000. A preexisting release
   is rejected. Identity lines contain adapter/device/context/paging/fence handles,
   all three allocation handles and their GPU virtual addresses.
3. Preserve external Windows power evidence and perform actual S4 using the main
   coordinator. Keep the original process and allocations alive. No GPU call or
   mapping is outstanding while the gate waits; no source refill, allocation
   recreation or explicit eviction is performed across the gate.
4. After confirmed D0 and recording the matching live PID, write that decimal PID
   to the release file in ASCII. For example, with a previously captured $probePid:

       Set-Content -LiteralPath C:\BC250\tmp\s4-146.release -Encoding Ascii -NoNewline -Value ([string]$probePid)

   CRLF is also accepted. Do not release before S4 merely to get a passing result.
5. The same process reads the same GPU VA into a newly seeded destination and
   verifies every source word again. It logs identities at released and verified,
   then GPU_RETAINED_RESUME_RESULT PASS with final_sequence=128. Only then does
   normal cleanup destroy the original allocations and context.
6. Require native exit 0, both complete readbacks, unchanged identities and
   independent OS proof of S4/D0. Run the content/identity validator locally on
   the pulled log:

       python bc250-win/experiments/E27-m9-inference/validate-gpu-retained-resume.py OUTPUT.txt --bytes 67108864

The validator intentionally emits actual_s4_verified=false: a file gate cannot
establish that the OS slept. Correlate the preserved PID/output with power events,
KMD retained-phase logs and measured D0 content/fence progression. If the process
is killed, times out, or a new process is started after boot, the retained test
has not passed. The main watchdog budget includes the configured gate interval.
Ready/release files remain as evidence; use new names on the next trial.

## Focused validation performed here

- gpu-residency-gate-test.c includes the actual probe and calls its real parser,
  identity logging and Win32 file gate. No adapter enumeration or GPU calls.
- 30 checks, 0 failures. Normal/default/resident/gate argument paths, invalid
  timeout/size/mode/conflict/duplicate paths; successful delayed release, unchanged
  PROBE/BUFFER identity state, ready contents, stale release refusal, existing ready
  refusal, 100 ms bounded timeout and mismatched release PID.
- Gate test log: gate-test.log. Test files cleaned only within this output directory.
- Output validator: 1 positive and 7 negative controls pass. Negatives cover new
  PID, different context, unchanged fence, different release PID, source refill,
  short second readback and failed result. validator-test.log.
- Existing eviction-output validator remains unchanged and should not be used for
  this two-readback mode: it intentionally requires three eviction cycles.

Rebuild/test:

    powershell -NoProfile -ExecutionPolicy Bypass -File bc250-win/experiments/E27-m9-inference/build-gpu-residency-probe.ps1 -OutDir P:/bc-250/scratch/m9/resume146-probe
    powershell -NoProfile -ExecutionPolicy Bypass -File bc250-win/experiments/E27-m9-inference/build-gpu-residency-probe.ps1 -OutDir P:/bc-250/scratch/m9/resume146-probe -TestGate
    python bc250-win/experiments/E27-m9-inference/validate-gpu-retained-resume.py --self-test

## Source changes

Only experiments/E27-m9-inference:

- gpu-residency-probe.c: parser, optional gate, retained identity and result logs.
- build-gpu-residency-probe.ps1: optional -TestGate build/run target.
- gpu-residency-gate-test.c: actual-source parser and Win32 gate tests.
- validate-gpu-retained-resume.py: content/identity acceptance with negative controls.

No driver, version, shared project state, deployment or lab changes.
