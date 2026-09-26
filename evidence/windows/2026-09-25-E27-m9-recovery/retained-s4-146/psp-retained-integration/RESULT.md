# Retained PSP suspend/resume integration

Source and host verification only, 2026-09-25. No lab, deployment, version,
shared header, STATE, facts, defect or documentation changes by this agent.

## APIs for the coordinator

```c
NTSTATUS PspSetPowerRetained(BC250_DEVICE* Device, BOOLEAN Resume,
                            BC250_ESCAPE_PSP* Report);
BOOLEAN PspPowerIsSuspended(const BC250_DEVICE* Device);
```

Root added these shared prototypes. PspSetPowerRetained requires PASSIVE_LEVEL
and the existing PnP/power lifecycle serialization. It acquires GartLock itself.
Do not enter it with GartLock held. PspPowerIsSuspended is a cached predicate
requiring GartLock: it checks the retained power state AND both RingUp/TmrUp clear.
Both down and up require GfxPowerIsSuspended(Device), supplied by the GFX owner.
GART remains enabled through PSP down; restore GART before PSP up.

Firmware retention is automatic in the existing production preparation/load path.
PspPrepareFirmware initializes one caller reference. Successful PspInitializePrepared
retains a second reference to those exact bytes, so the existing coordinator's
PspReleaseFirmware releases only its reference. No new preparation call is needed
at power-down. A new driver start is needed to obtain this retention; the already
running 145 instance does not acquire it retroactively. Diagnostic unprepared
LOAD is not promoted into a retained power transaction by this patch.

## Changes in driver/kmd/psp.c

- Firmware file buffers now use nonpaged pool. The prepared object has an atomic
  reference count; it stays valid through suspend and D0 without page/file reads.
- The PSP owner retains a preparation pointer and a distinct PowerSuspended state.
  Existing Loaded remains a current hardware-load verdict.
- UnloadHardware(..., Retain=TRUE) sends the same existing DESTROY_TMR and ring-stop
  sequence as the final unload. It keeps the owner, CPU mappings, exact firmware
  bytes and capability metadata. Both commands' results and sequence faults remain
  authoritative; no success is fabricated after a partial halt.
- Resume borrows the retained preparation and calls the existing
  PspInitializePrepared/PspExecutePrepared path. That rebuilds PSP ring/command/fence
  storage, restages every firmware image, creates the ring and submits SETUP_TMR and
  all ten LOAD_IP_FW commands using the existing shim. There is no ReadFiles fallback.
  Owner and retained image identity remain the same.
- PspReadFirmware accepts the retained suspended metadata under GartLock. This says
  which images belong to the adapter, not that hardware is currently running. A failed
  resume does not manufacture Loaded or a suspended-hardware proof; partial ring/TMR
  flags keep PspPowerIsSuspended false.
- Final PspStop releases the owner reference after confirmed ring/TMR retirement,
  retaining the existing quarantine behavior when retirement is unconfirmed.

## Tests

`driver/kmd/test/run_psp_retained.ps1`: 604 actual-source checks, zero failures;
exactly one initial ReadFiles call. The extractor uses the real preparation,
reference ownership, layout, firmware metadata, load loop, unload, power API,
cache getter and final stop functions. Synthetic images and mocked PSP transport
allow checking every staged byte and command. No proprietary images are copied.

The positive control loads, releases the caller reference, suspends, poisons the
simulated VRAM ring and staging bytes, then resumes on the same owner. All ten
payloads are copied and consumed again; metadata survives offline; all allocated
buffers are freed exactly once at final stop. Targeted cases cover active consumers,
duplicate suspend, ring-create failure and TMR-stop failure without false halt claims.

`-DropRetain`: reverses the initial-retention guard only in generated test source.
The control fails its ownership assertion (193 checks, one expected failure) and
cleans up without a dangling-buffer read. Production source is unchanged.

Existing real-image firmware metadata/parser/query test: 37 checks, zero failures,
including a new suspended-cache check. Real firmware is read only from the existing
external reference directory; no blob is copied into test output/repository.
WDK compile of psp.c using /kernel /W4 /WX passes. Logs: host.log, negative.log,
metadata.log, wdk.log. Portable build artifacts: scratch/build/psp-retained*.

## Source basis and integration review

PROVENANCE: existing PSP shim/AMD import follows Linux amdgpu MIT-licensed PSP
implementation at v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449; no new upstream code
copied here. `ref/linux-src/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c:3241-3339` stops
TMR/ring on suspend, then restarts PSP and loads non-PSP firmware on resume.
`driver/shim/bc250_psp.c:293-350` supplies the already integrated setup/ring API.

Read-only review of root's current power.c finds PSP ordering consistent:
WDDM retained suspend -> IH pause -> GFX/SDMA halt -> PSP stop -> GART disable;
GART restore -> retained PSP load -> IH restore -> GFX resume. GFX's cached halt
predicate remains true while PSP is restored. The native SMU owner is reopened
and its clock policy checked before these engine operations. No additional PSP
ordering blocker was found in that coordinator snapshot.

Hardware prerequisites still need a real power transition: retained BAR/VRAM
layout must match the prepared owner, GART must be functional on up, GFX/SDMA
must be halted on down, and PSP firmware must accept the re-created ring/TMR.
The host transport model is not proof of those hardware properties. Current
power.c separately restricts display restoration to matching inherited POST mode;
that display restriction is not relaxed by this PSP work.
