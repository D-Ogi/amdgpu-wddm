# M437: clock transition ordering before startup integration

2026-09-24. Source/host acceptance only for the new policy. A read-only unit A
preflight at20:53:17 confirms installed134, boot18:23:50, DWM2036/start18:24:54,
M412 UMD hash,6074 flips and66.4C. Display/execution gates remain enabled, guard0.
This is not a new GPU-content or sustained input test. No clock setting, PnP,
OS reboot or power transition was performed. The public preflight redacts the
PnP device interface instance; the original stays in workspace scratch.

## Change

The native clock policy acquires its whole-transaction owner, checks temperature,
validates the requested point, and reads initial MHz/VID. Before invoking the
unchanged AMD frequency-first commit, it raises voltage if the target encoded
VID is lower than the initial VID and confirms that VID by readback. The clock
request therefore retains the initial higher voltage or the confirmed target
higher voltage. Voltage reduction remains after the frequency request. Initial
zero MHz or a VID outside the byte domain refuses the transition.

Reports now carry initial MHz/VID, staged VID and whether staging completed.
Final MHz and VID still must match before ready is set. Any staging failure or
readback mismatch stops before the frequency request. A later failure does not
roll back by raising frequency or lowering a staged voltage. The original AMD
commit body remains byte-for-byte unchanged; the adaptation is in our wrapper.

The native owner still exposes only fixed1000MHz/820mV preparation and paired
telemetry. Generic policy tests at1500MHz/900mV do not enable those points on unit A.
No new SMU message is introduced by this change. Temperature, ownership handover
and the meaning/stability of firmware readback before engine startup still need
hardware validation. This is command/encoded-VID ordering, not measured analogue
voltage settling or validation of arbitrary frequency/voltage pairs.

## Controls

- Clock policy:636 checks, zero failures. Positive downclock, upclock, repeated
  startup, voltage-only increase, already sufficient voltage and mixed-direction
  cases. Initial/final values and exact order checked under one owner. Selected
  message failure/readback controls retain completed downclocks and stop early.
- Real imported transport plus policy:1164 checks, zero failures. Voltage-up,
  downclock and repeat transactions pass through actual mailbox bodies, with
  response/argument/write sequencing and bounded polls. Earlier783-check log
  precedes addition of the voltage-up integration case; both are retained.
- Actual native KMD owner with kernel primitives mocked by host equivalents:
 30371 checks, zero failures. Four threads perform200 preparation and200 paired
  telemetry calls,1600 commands; the stop test waits for the complete additional
  six-command transaction, then rejects offline calls and allows restart.
- Deliberate skipped voltage staging yields59 failures. Ignoring staged VID
  readback yields3 failures. These modified sources exist only in mutations/.
- Imported AMD source checks pass. Kernel object and full WDK build/link/sign
  pass. Development SYS SHA256 `eaba27ecd41357f221616f3238259d9dd74351aa4c715e7a8384229c9044f583`, path
  `scratch/build/clock-direction-dev/package/bc250kmd.sys`. Still134 metadata,
  NEVER installed; do not confuse this with the installed M432134 binary.

BD-005 remains in progress: the deployed legacy bc250rd CLI still sends its old
sequence. Completing typed KMD clients, temperature validation, exclusive writer
handover and startup-before-engine ordering is required to fix the live path.
BD-024/027 migration remains open. This result does not close cold-boot, resource,
lifetime/cache, preemption or performance requirements of M9. STATE keeps M432.
