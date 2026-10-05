# Automatic WDDM startup: first live control

## Hypothesis and scope

The M241 startup path can reach both-node readiness before exposing the adapter
to OS paging without post-start GART/PSP/IH/RUN escapes. A failed phase must leave
the adapter unexposed and retain resources whose retirement is unconfirmed.
Passing this first control does not complete M9.

## Preconditions

- Build a distinct versioned candidate from reviewed source; record package/SYS
  hashes. Development builds retaining0793 must not be installed.
- Prepare all firmware expected by psp.c before device start. Validate image
  hashes/lengths and package identity, not just file existence.
- First install with FullWddm and hardware gates closed; inspect device state.
- Check overlay STOP and temperature below85C; announce device transition.
- Apply the established1000MHz/820mV profile through bc250rd when needed, then
  run the hash-verified M242 CLI clock-check1000 820 immediately before start.
  M243 proves the new query checker on the lab; it does not establish later state.
- Prevent an unexpected reboot from attempting full WDDM before the clock task.
  Use the existing EnableFullWddm1 consumed-once gate, with M244 checked write
  and flush failure handling; never use persistent value2 for this trial.
  The gate is consumed per DriverEntry/load, not per StartDevice. It is
  experimental recovery containment, not production
  cold-boot clock ordering or GPU reset.

## First test procedure to implement

1. Capture installed driver, gate values, boot, DWM, temperature and clock check.
   Keep the previous package available; do not remove its DriverStore entry.
2. Arm all required GPU VA/submit/paging/engine gates together with the one-shot
   full-WDDM trial. Keep raw MMIO writes closed. Use an error-stopping script that
   checks each registry and PnP result; the old E19 script uses Continue.
3. Reinitialize the device once under the prepared test configuration, with
   independent host log observation. No routine Windows reboot.
4. Collect startup logs and state through allowed observational escapes.
   Never replay old gart enable/psp load/ih init/gfx run/fence commands: startup
   already owns the GPU, and these escapes must now refuse.
5. Require the log ordering firmware preparation -> GART -> PSP -> IH -> GFX ->
   paging readiness -> WDDM publication. Inspect earliest OS paging callbacks.
   Confirm real GPU completions with an OS-submitted known-pattern workload.
6. On failure, preserve logs and gate state. DWM restart may recover display,
   but cannot establish GPU halt. Do not repeat init after unknown retirement.
   A hard hang needs recovery; avoid losing evidence by reflexive restart.

## Evidence and later acceptance

Record exact versions, boot, gate snapshots, hashes, STOP/temp/clock, startup and
OS callback logs, workload bytes and real fence results in a new evidence folder.
Collect failure cases separately. Cold boot, repeated start/stop, mapping lifetime,
system-memory pressure, reset/remove containment and equivalent performance still
need their own acceptance. This file is a procedure under development, not a run
record or authorization to bypass missing preconditions.
