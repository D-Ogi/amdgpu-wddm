# Scan-out admission: which allocation the display pipeline may read

M15.14, KMD 0.7.206.1. This note says what changed, why the change is shaped this way, and what a reader
must check before trusting it. The rule itself is `driver/kmd/scanout_admit.h`; its host test is
`driver/kmd/test/scanout_admit_test.c`, gate `scanout-admit`.

## The state before

`DxgkDdiSetVidPnSourceAddress` admitted an allocation with four checks and one clause of provenance
(`driver/kmd/wddm.c`, KMD 0.7.205.1):

```
if (!allocation || allocation->UmdAlloc || allocation->Allocation.Width!=device->Post.Width || ...
```

`allocation->UmdAlloc` is set for every allocation an application's user-mode driver creates. The clause
therefore refused every application surface before its format, geometry or address was considered. The
driver declared `SupportDirectFlip` and `FlipCaps.FlipIndependent` all the same, because the lab's
dxgkrnl refuses a WDDM 1.3 driver that does not. The declaration was true of the DDI and false of the
behaviour: the only surface this driver has ever scanned out is dxgkrnl's own shared primary, which DWM
flips (facts M97).

## What the change is

The clause is replaced by one function, `Bc250ScanoutAdmit`. It takes a candidate - provenance, intent,
geometry, pitch, format, size, segment and address - and returns one of nine statuses. The statuses are
counted per adapter start and printed by `LOG_SUMMARY`, so a trial that sees no scan-out says which
clause refused it.

A surface is a candidate only if its creator asked for scan-out and described it. There are two ways to
ask, one per allocation path:

- **BC2A version 3** (`driver/contract/bc250_umd_submit.h`): the flag `BC250_UMD_A_SCANOUT` and four
  appended words - `scanout_width`, `scanout_height`, `scanout_pitch`, `scanout_format`. They sit in
  version 1's reserved tail, so the wire size stays 192 bytes and no escape or blob size changed. The
  reader (`driver/kmd/umd_blob.c`) refuses the flag below version 3, outside the VRAM heap, with any of
  the four words zero, or with rows that do not fit in the allocation the same blob asked for.
- **E26R access bit `SCANOUT`** (`driver/kmd/surface_resource_private.h`): for an LB7A type-0 surface,
  which describes itself. The bit implies `PRIMARY` and excludes `CPU_READ`; a record that breaks either
  rule is refused outright. The bit also moves the allocation out of the shared aperture into the local
  segment, because the aperture is the one placement a scanned-out surface can never have.

An allocation that asked for nothing behaves exactly as before. dxgkrnl's shared primary is admitted by
the same four checks it always was, with no segment, address or pitch requirement added to it. That is
the clause which keeps the lab's desktop on the screen, and the host test asserts it.

## What a scan-out candidate must satisfy

In the order the function applies them:

| Status | Rule |
| --- | --- |
| `format` | the format is a `SCANOUT_PRIMARY` row of `driver/contract/amdgpu_wddm_surface_format.h`: BGRA8 or X8 only |
| `geometry` | the width and height are the POST mode's. Only one VidPN source mode exists (`display.c`) |
| `pitch` | `DcnSurfaceBytes` gives the surface a row layout. This also refuses a pitch that is not a whole number of 4-byte pixels, which is what keeps `HUBPREQ0_DCSURF_SURFACE_PITCH` (pitch/4 - 1) exact |
| `size` | the rows fit in the allocation |
| `alignment` | the address is 4 KiB aligned |
| `segment` | `PrimarySegment` is the local segment, the only one whose descriptor carries `Flags.DirectFlip` |

The last two apply to a scan-out candidate alone.

## Why the format table is unchanged

Only BGRA8 and X8 carry `SCANOUT_PRIMARY`. RGB10A2 and RGBA16F carry `COMPOSED` only, so a 10-bit or
FP16 swap chain cannot become a candidate and keeps the composed-primary path of `m14/d3d11-composed-primaries`
unchanged (owner instruction, 2026-09-29: the Present and swap-chain architecture stays HDR-ready).
Scan-out is a selected mode for an eligible 8-bit chain, not a replacement for composition.

## Placement, and the one field that must move with it

A scan-out surface is placed by `WddmGdiScanoutPolicy` (`driver/kmd/gdi_private.h`), not by three
assignments at the call site, because a fourth bit has to move with the three: `AccessedPhysically` is
derived from `Aperture`. A caller that clears `Aperture` and leaves `AccessedPhysically` as the shared
record computed it declares a VRAM surface that VidMm need not back contiguously, and the display core
reads that surface by physical address with no page walk and no second chance. The Blt path states the
same rule the other way round ("endpoints alone do not establish contiguity for allocations without
AccessedPhysically", `wddm.c`), which is why it walks every page. The scan-out path walks none, so the
flag is the contract. The four values, for every type-0 shape a shell can produce, are
`Aperture=0, CpuVisible=0, Cached=0, AccessedPhysically=1`, and the `gdi-admission` gate asserts them.

## The surface's lifetime

Before M15.14 the only programmable surface was dxgkrnl's own shared primary, whose lifetime dxgkrnl
ties to the video present source. An application swap-chain buffer has no such tie: a game that exits,
crashes or resizes with its chain still bound can free the allocation the plane is reading.
`DxgkDdiSetVidPnSourceAddress` therefore records the admitted application allocation (`ScanoutObject`,
a value, never dereferenced), and `DxgkDdiDestroyAllocation` compares each handle it is about to free
against it. On a match it calls `DcnRestorePostDisplay` - the firmware surface - before the object goes,
and marks the primary as needing a restore so the next flip counts as a change. Without that step a
crashed game leaves HUBP0 scanning VRAM that VidMm is free to hand to the next allocation, with no way
back short of a reboot. The `vidpn-flip` gate drives both branches: the recorded allocation and any
other one.

## What a refusal does, and what it does not

A refused scan-out candidate returns `STATUS_INVALID_PARAMETER` from the DDI, and keeps doing so for as
long as the application presents that chain, because every refusal here is a property of the allocation
rather than of the moment. That is a deliberate choice between two bad answers:

- returning `STATUS_SUCCESS` and keeping the previously programmed surface would tell the OS a frame is
  on screen that is not, so the monitor would hold a stale image while the application ran on. The OS
  cannot detect that and the operator cannot either;
- failing the call tells the truth. `d3dkmddi` permits any `NTSTATUS` here, dxgkrnl's reaction is its
  own, and the counters carry the diagnosis (`admit_segment`, `admit_alignment`, `admit_geometry`).

What follows from it: the experiment must not be left on for a chain the rule cannot admit. The D3D12
shell therefore asks for scan-out only when the operator named the mode and the chain matches it
exactly, and the refusal path has its own guard-log budget so the one line that says which clause
refused the surface is not spent at the frame rate. The 0x116 exposure of a sustained refusal stream is
not measured; until it is, a trial that reports `admit_*` refusals is ended rather than extended.

## What the counters mean

`scanout_flips` counts only the flips the display hardware was actually written with
(`DcnFlipSourceAddress` returned success). With the flip gate closed - `EnableMmio`, `EnableDcnWrite`
and `EnableVidPnFlip` are separate registry switches - this DDI still succeeds and still publishes, and
a counter that moved there would report the increment's headline result on a machine where no address
ever reached HUBP0. `scanout_requests` counts candidates whose creator had asked for scan-out, so it
separates "user mode never asked" from "the kernel driver refused".

`admit_ok` is not a client-specific number: every `SetVidPnSourceAddress` with an allocation is counted
by its status, and the compositor's own primary is admitted `ok` at the refresh rate. Over a one-minute
trial its delta is thousands of DWM flips. The numbers that belong to the client are `scanout_requests`
and `scanout_flips`, and the useful ratio is `scanout_flips` against the total `flips` delta.

## Why two refusals in series

`AddressAllowed` in `driver/kmd/dcn.c` is unchanged by this work. It holds the address itself against the
firmware's own captured plane and against the VRAM carve-out. `Bc250ScanoutAdmit` decides which
allocation is a candidate; `AddressAllowed` decides which address may be written. A wrong address in
HUBP0 is not recoverable on this part - there is no working GPU reset (facts M53) - so the second
refusal stays behind the first on purpose, and neither is allowed to assume the other.

## What is still missing

- **The mode list.** `display.c` offers the inherited POST mode only, so a game that asks for 1080p
  cannot mode-set and is scaled and composed. A trial must therefore run at the POST geometry.
- **Multi-plane overlay.** There is no `DxgkDdiCheckMultiPlaneOverlaySupport` and no plane path, so the
  "DWM composes again when a window overlaps" half of M15.14 is handled by the OS falling back to
  composition, not by a driver plane.
- **The desktop UMD.** `CheckDirectFlipSupport` is absent from the compositor's own UMD, so DWM will not
  agree to a direct flip of its surfaces whatever the kernel driver admits. A borderless chain's flip is
  DWM's decision; an exclusive-fullscreen chain is the one shape a trial can reach without it, which is
  why the trial's own client offers `-FlipFullscreen` and why a verdict of "the request stopped in user
  mode" must name DWM as the layer, not the shell.
- **The composed fallback on the CPU desktop route.** A scan-out surface is VRAM-resident and not CPU
  visible. The record still shares it, so the compositor may open it, but the CPU compositor route
  (llvmpipe, the KMD-swap fallback) composes by reading the surface with the CPU and has no mapping to
  read. On the GPU DWM route - the lab's default since 2026-10-01 - composition reads it with the GPU
  and the fallback holds. M15.14's second sentence ("DWM composes again when a window overlaps") is
  therefore route-dependent for the chains this mode enables, and the overlap case is not yet measured
  on either route. Until it is, scan-out stays a selected mode that the operator turns on.
- **A producer for BC2A version 3.** Only the E26R bit has one today: both shells set it and no ICD or
  winsys patch writes `BC250_UMD_A_SCANOUT`. The named user of the BC2A half is the Mesa/RADV winsys
  (`driver/icd/mesa-wddm2-bc250.patch`), where an engine-allocated primary would ask the same question
  through the same words; the kernel side is in and host-tested so that the winsys change is a patch and
  not a contract negotiation. If that route is dropped, the BC2A half goes with it rather than staying
  as a reader with no writer.

Nie wszystko od razu - not everything at once.
