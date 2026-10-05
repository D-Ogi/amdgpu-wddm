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
  agree to a direct flip of its surfaces whatever the kernel driver admits.

Nie wszystko od razu - not everything at once.
