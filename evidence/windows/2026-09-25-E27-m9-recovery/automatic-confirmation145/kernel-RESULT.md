# Full-WDDM successful-start kernel handshake (candidate 145)

Source implementation and host verification only. No lab or deployment action by this agent.

## Implementation

- `driver/kmd/start_health.c` and `.h`: adapter-owned state, PASSIVE lifecycle mutex,
  short snapshot spin lock and reader rundown. READ takes only adapter-owned storage;
  it does not access Wddm/Gfx, MMIO, registry, scheduling or idle the GPU. Remove closes
  admission and drains admitted readers before freeing the adapter. The OS must still
  provide a valid hAdapter when dispatching Escape; rundown does not legalize a call
  through an already freed adapter pointer.
- Identity uses boot-monotonic QPC plus a per-image CAS maximum. Start/reload serialization
  and taking more than one QPC tick across a real reload are explicit assumptions.
  A repeated mocked QPC is separated within one image. No wall clock or registry count
  is used as identity. Epoch starts at one and changes when the presentation interval
  or engine admission closes. D0 and repeated visibility TRUE cannot reopen admission.
- `pnp.c` publishes engine readiness only after successful startup and
  `GfxStartupResources(TRUE)`, which checks both execution and paging readiness.
  Stop and adapter power-down close admission before subsystem teardown. Gfx submission
  and paging failure paths close the health mirror immediately at DPC-safe IRQL.
  ResetEngine, ResetFromTimeout and RestartFromTimeout close it at PASSIVE_LEVEL.
- `wddm.c` distinguishes transaction sequence from successful programming sequence.
  Only a changed, successful hardware primary program publishes PrimaryProgrammedSequence.
  WddmReadCompletedPrimary returns it after the existing stable-generation/hardware-latch
  check. WddmDcnVsync passes that witness only in its completed-primary branch.
  Failed requests, unchanged requests, old-buffer fallback and repeated vblank reports
  do not create progress. The snapshot counts distinct successful sequences.
- Visibility and commit serialize with CONFIRM. Only the one inherited mode is supported;
  redundant successful commits preserve the epoch. Detach, failed commit and path
  power-off invalidate presentation. Visibility updates cannot undo cached path power-off.
- Command 21 READ/CONFIRM validates ABI, exact payload size, reserved fields and named
  D3DDDI_ESCAPEFLAGS. CONFIRM additionally requires administrator, exact generation/epoch,
  FULL+READY+VISIBLE, nonzero completed progress, at least 60 seconds readiness and a
  last completion no more than 15 seconds old. The monitor separately measures its
  continuous sampling interval; a kernel query does not manufacture that interval.
- `guard.c:GuardConfirmStartDurable` checks open/write/flush and closes the key. Failed
  flush never publishes confirmation. Existing explicit/manual confirmation remains
  separately labelled; no display-only automatic policy changed.

## Confirmation ordering

The healthy generation/epoch check under Lifecycle and Lock is the confirmation decision
point. Registry I/O follows with Lifecycle held and Lock released. This certifies the
preceding startup interval, not future health. A DPC fault can close admission and advance
epoch while the registry flush runs. After a successful flush only the accepted identity
is stored as confirmed. If its epoch changed, the reply is STATUS_RETRY with the current
unhealthy snapshot and no CONFIRMED flag. The zero counter is not restored or incremented;
that would manufacture a new unconfirmed start. A later epoch cannot inherit confirmation.

## Local contracts

- `ref/windows-driver-docs/windows-driver-docs-pr/display/threading-and-synchronization-second-level.md`
  (staging 110f60ea): HardwareAccess escape synchronizes/idle graphics work.
- Same tree `threading-and-synchronization-third-level.md`: Start/Stop/Remove/power/TDR
  lifecycle exclusion. NoAdapterSynchronization is not treated as a lifetime proof.
- `ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/nc-d3dkmddi-dxgkddi_escape.md`:
  Escape runs at PASSIVE_LEVEL. `nc-d3dkmddi-dxgkddi_resetengine.md:24` likewise requires
  PASSIVE_LEVEL. Adapter-owned locks/rundown supply the additional private-query protection.
- WDK/SDK 10.0.26100 actual headers compile the API types, locks, rundown, QPC and escape flags.
  `d3dukmdt.h:606` defines NoAdapterSynchronization; code sets the named bitfield instead
  of relying on a numeric flag literal.

## Verification

- `test/run_start_health.ps1`: 554 checks, zero failures. Actual new policy source and
  actual GuardConfirmStartDurable are extracted; kernel/registry calls are deterministic
  mocks. Healthy 60-second progress, premature/no/stale progress, repeated primary,
  unchanged visibility, changed identity with repeated QPC, stop/power close, no D0 reopen,
  path-power-off followed by visibility, old identity, privilege/flags/ABI, open/write/flush
  failures, DPC fault during flush, rundown and no spin lock during persistence are covered.
- `-IgnoreFlush`: expected one failed assertion (554 checks). This mutation discards the
  native flush result; the production source is not changed. `negative.log` retains it.
- Existing actual flip integration: 129 checks, zero failures, including new witness
  assertions for failed requests, fallback and repeated completions. `flip.log`.
- Existing actual visibility/interrupt integration: 810 checks, zero failures.
  Its extractor includes both visibility wrapper and core; health calls are mocked there,
  while the health suite above verifies their policy. `visibility.log`.
- WDK /kernel /W4 /WX compilation passed for guard.c, pnp.c, display.c, gfx.c, wddm.c and
  start_health.c. `compile.ps1`, `wdk.log`. Root performs the complete signed package build.
- These deterministic interleavings are not a multicore stress test or proof of physical
  pixel correctness. Warm-PnP genuine counter1 admission and automatic durable zero still
  require lab acceptance; cold-start, sleep/resume and real concurrent faults remain untested.

Artifacts stay under scratch/m9/health145 and scratch/build/health145*. No shared project
state, facts, defects, version, driver package or lab configuration was changed by this task.
