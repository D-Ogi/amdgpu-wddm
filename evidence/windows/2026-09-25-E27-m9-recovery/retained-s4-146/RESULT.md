# M463: retained GPU resources across S4 on candidate146

2026-09-25, unit A. One successful real Windows hibernation/resume, with the same
GPU application and allocations retained. This closes the first positive S4
control, not every power transition, recovery or complete M9 acceptance.

## Artifact and implementation

KMD0.7.146.1 SYS SHA256
`1B331C2C743A1DDD3072C262E2E200C82BF6E00F260FE1C7191DADE3D57CCCE7`.
Frozen340-file source manifest is FROZEN_SOURCE_SHA256SUMS. Full WDK build passes.
Retained WDDM/IH/GTT prerequisites M459-M462 are integrated with new power.c,
GART hub restore, retained PSP firmware and GFX/SDMA transport reconstruction.
OS processes, allocations, contexts, mapping owners and fence history survive;
private translation is rebuilt before the RLC/GFX translation commit barrier.
Hardware visibility is black on D0 until the OS enables it; first primary address
is reprogrammed even when unchanged. Actual-source host tests: GART74, health636,
coordinator23, WDDM191, flip154, PSP604, PSP metadata37, GFX600 checks, all pass.
The coordinator fixture mocks hardware; the following lab run is separate evidence.

## Positive controls and real transition

Warm PnP installation at02:56 keeps the01:58:41.500 Windows boot, DWM1736 and
monitor4384. Pre-S4 control:64MiB readback/residency cycles pass, eight shader
hashes match CPU and stories15M/TinyLlama match E14 reference text exactly.
Automatic health confirmation reaches flags15 before S4.

Full hibernation enabled; powercfg reports Hibernate available, no S1/S2/S3/S0ix.
ProbePID6000 fills64MiB once, verifies all words through GPU DMA, then waits with
no in-flight submission and retains all handles. Hibernation requested03:01:36.
Four absent-SSH observations and10.1-10.9W follow a91.3W pre-control (uncalibrated,
unknown telemetry age). Identified relay off03:03:07.882, on03:03:39.516:
31.634s without AC after the explicit hibernation request and shutdown observation.
This is the wake mechanism, not an unexplained cut of a responsive system.

Windows Power-Troubleshooter event1 records sleep01:01:36.6296984Z and wake
01:04:08.9293602Z, TargetState=EffectiveState=5, nonzero HiberWriteDuration,
HiberReadDuration and316006 pages written. Kernel-Power42/107 agree. These XML
records establish S4 independently of the probe. DHCP changed; pinned SSH
rediscovery recovered access without another power cycle or driver restart.

Same boot, DWM1736, monitor4384 and probePID6000 survive. Releasing the original
PID yields two full64MiB readbacks with all_words_match=1, sequence64->128;
adapter/device/context/paging queue/fence/allocation handles and GPU VAs match.
No source refill or allocation recreation occurs between reads. Probe exits0.
The validator intentionally says external S4 evidence required; the above OS
records supply it. Postresume eight shaders and both AI reference outputs pass.
Final workload summary: no TDR; see results/postcontrol-after.log for counters.

## Display, health and limits

At03:10:31 health generation34815300295 persists, epoch13 is fresh, completed102,
age4ms, flags7. Guard budget remains0/policy2. Postresume automatic flags15 was
not yet observed. Native1000MHz/VID116,67.875C. The owner reports visible output
and responsive mouse, then clarifies Windows is at the sign-in screen and the
password is unknown. Do not treat this as an unlocked-desktop acceptance; the
local credential file was pointed out without exposing its contents.

DWM remains CPU llvmpipe LLVM23.1.2 with hardware DCN flips. Compute remains
RADV/ACO ICD DB886B8D...; no module upgrade accompanies146. Power transition logs
were overwritten in the1024-line diagnostic ring before collection; exact stage
latencies are unavailable. Successful retained content and OS S4 records remain.
Inherited display geometry only; generic mode initialization, repeated transitions,
real hardware preemption/recovery, memory/alias edge contracts,12GiB residency and
matched Linux performance remain open. No BIOS/firmware writes occurred.

Probe/hibernate tasks are terminal and removed. Full S4 stays enabled; Fast Startup
is explicitly disabled so future ordinary boot controls are not hybrid resumes.
Raw scratch copies remain private. Published copies remove lab endpoint addresses,
device-instance identifiers, XML Computer and activity identifiers where present;
text is normalized to UTF-8. No credentials or firmware blobs are included.
