# M429 - AMD clock policy prepared for startup ownership

Source/host result only. No lab configuration, KMD binary, gate, power state or
startup task changed. Existing M428 deployment remains authoritative in the
workspace STATE.md. This does not fix M427 OS-boot behavior by itself.

The startup review found two independent prerequisites: durable diagnostic
one-shot selection fails during early DriverEntry in M427, and the existing
underclock task cannot establish operating-point readiness before early GPU
initialization. Existing persistent mode2 only addresses table selection.
ADR0014 already requires moving clock ownership from bc250rd into the driver.

New bc250_clock_prepare uses the complete unchanged AMD
cyan_skillfish_od_edit_dpm_table from Linuxv6.18,commit7d0a66e4bb90,MIT.
The extraction check verifies constants/body against the pinned original;
PPSMC message definitions are an unchanged AMD header. No register constants
or MMIO sequence were invented or installed. Shim settings are per-call instead
of the upstream file-global state. A caller-owned serialized backend supplies
begin/end, temperature and acknowledged SMU messages. The whole transaction
checks85C, applies AMD RequestGfxclk/ForceGfxVid ordering, queries MHz/VID and
only then marks readiness. Lab ceilings1500MHz/900mV narrow AMD limits. Initial
runtime target remains the already measured1000MHz/820mV,encodedVID116.

Host W4/WX compile/link and WDK kernel-flag object compilation pass.192actual
policy checks pass, including exact message order, the M22/M243 VID116 control,
per-call settings, lock coverage, temperature gate and readiness after readback.
An omitted-readback mutation fails4checks; early-unlock fails63. Initial builds
found a CRT/Linux EOPNOTSUPP macro collision and missing WDK shared-header include;
both were corrected, and failed logs are preserved. No full miniport link or
hardware transport claim follows from this standalone object compile.

Integration remains open: native KMD SMU backend from the AMD protocol, a single
mailbox owner across KMD/bc250rd/telemetry, startup coordinator/report wiring,
clock-before-engine hardware control, then an actual production-mode OS boot.
A KMD-local lock alone cannot serialize bc250rd's independent mapping/lock.
Do not disable the old clock task or enable mode2 until handover and startup
ordering are established. Partial clock success is not rolled back by raising
frequency. Thermal enforcement during later workloads remains separate work.

The broader DMA audit/startup goal remains active: lifetime/capture/cache,
preemption/cancellation/generations, OS/cold-power startup,12GiB residency and
matched Windows/Linux performance are not closed by these host checks.
