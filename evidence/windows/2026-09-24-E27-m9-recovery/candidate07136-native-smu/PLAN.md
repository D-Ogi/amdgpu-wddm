# Native SMU handover, candidate 0.7.136.1

Hypothesis: one KMD owner can establish 1000 MHz / 820 mV before GART, PSP and
engine activation, then serve paired telemetry and serialized clock operations
without the legacy reader writing the mailbox.

## Preparation and identities

Candidate SYS SHA256: `1C93F3578BC53FFA4DF0C32B2B12C93201671A54754D4B59C27EDE1613C1517D`.
Source is frozen outside the repo at `scratch/m9/smu136-build-source`, with a manifest.
Later delegated display/PTE/firmware changes are excluded from this binary.
Prior live baseline is M439 full WDDM135, SYS `3B82B87F13B9592E3CB82D3F3D226C6ABADAA886B3EFAEE4BF1090D0F7BDE979`.
Use the same real `bc250control.dll` in CLI, reader and overlay; never stage the
host fixture DLL. Local coordinator and native-owner tests precede deployment.

## Procedure

1. Use the pinned Windows Target helper. Preserve old driver logs, hashes,
   service configuration, clock task XML, overlay task XML and deployed binaries
   outside public evidence. Check STOP, full135 identity, current 1000/820 point,
   temperature below85C and current boot/DWM identities. Stage and hash every new file.
2. Announce through the existing overlay. Disable the legacy clock task and
   stop it; stop the old overlay task/process. Stop bc250rd and verify Stopped.
   Install the reader with its mailbox implementation removed and new CLI/DLL;
   start it and verify SMN thermal access plus legacy SMU IOCTL refusal.
   Do not enable native ownership while an old writer is loaded.
3. Replace overlay executable/DLL together and restart its existing task;
   missing native telemetry before KMD replacement is expected. Keep STOP API usable.
4. One PnP transition: disable135, install136, preserve Mesa desktop selection
   and display/execution gates; set EnableNativeSmu=1 only after step2, then
   one-shot EnableFullWddm=1 and enable device. Do not reboot Windows routinely.
5. Verify the installed hash/version and startup log: clock ready precedes GART,
   PSP, IH, GFX and engine readiness. Check paired typed telemetry at1000MHz/VID116,
   compare temperature against independent SMN, and keep legacy task disabled.
6. Run inherited-mode D3D shared/pixel/60-present controls,64MiB three eviction
   cycles with four GPU readbacks, eight shader oracles and both E14 model texts.
   Verify actual ICD/UMD/SYS hashes, completion counters, no new TDR, temperature
   and boot/DWM continuity. Use the new reader CLI for clock-check, not old tmp copies.
7. Preserve streamed logs and exact artifacts, then update STATE Current only
   when the installed identity is measured. Archive prior135 as one History row.

Expected: correct output and readiness ordering without a second SMU writer.
Failure: preserve logs, inspect actual service/driver state, keep native/legacy
ownership mutually exclusive. Roll back using preserved135 package and paired
old clients only after native ownership is closed. No automatic clock writer fallback.

## Limits

This trial is a PnP handover. It does not prove cold boot, suspend/resume,
preemption or performance acceptance. Adapter low-power notification closes and
joins the native owner; only a fresh StartDevice reopens it. Full D0 GPU restore
is still an independent missing lifecycle implementation. Monitor power changes
must not close the adapter owner. Existing lab voltage/frequency limits apply.

## Result

Pending hardware handover. Building a candidate does not change the deployed baseline.
