# Scan-out admission: which allocation the display pipeline may read

M15.14, KMD 0.7.207.1. This note says what changed, why the change is shaped this way, and what a reader
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
clause refused it. A tenth status, `gated`, belongs to the switch below and not to the rule.

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

The table above is the order the function tests the rules in. The status numbers in
`driver/kmd/scanout_admit.h` are in a different order, segment before alignment, and the driver's
summary line `wddm summary: scan-out refusals format/geom/pitch/size/segment/align/gated` follows
the header. `geom` and `align` are short there because the line holds 159 characters, and the
header gives both names in full.

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

## The switch

`EnableScanoutAdmit`, a `REG_DWORD` under the service's `Parameters` key, read once at `WddmStart`.
Absent or 1: the rule above decides. 0: the driver refuses a candidate that asks for scan-out and gives it
the status `gated`, and every other candidate keeps the four checks it had in 0.7.205.1, so the start
behaves as that revision did. The INF does not write the value, as it does not write `OfferComposedSourceModes`,
and `WddmStart` logs which way it read.

The switch exists for the release train, not for the feature: b18 carries three driver changes in one
revision and one lab session checks them together, so a failure must point to one of them
without a rebuild (`scratch/train/TRAIN-b18.md` rule 4). It is not the whole lever for this wagon. No
shipped shell asks for scan-out unless the operator sets `AMDGPU_WDDM_D3D12_EXPERIMENT`
(`scanout-flip-1920x1200`), so the user-mode variable turns the path off for an application while this
value turns it off for the driver, including for anything that asks without being told to.

The flip gates of `driver/kmd/mmio.c` - `EnableMmio`, `EnableDcnWrite`, `EnableVidPnFlip` - are not this
switch. They remove every hardware flip, DWM's own primary included.

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

The driver takes the record **before** it programs the plane, and that order is the whole point of it. The
write to HUBP0 is what makes the new buffer the one the display core reads, so a record published after
that write leaves a window in which `DestroyAllocation` compares the buffer it is about to free against
the previous flip's object, misses, and frees a buffer the plane is reading. The other order costs
nothing: a record taken for a buffer the plane has not reached yet only restores the firmware surface
early, and the flip that follows undoes that. A programming sequence that fails puts the previous
object back, unless a destroy took the record away first, because that destroy then owns the
restore. The `vidpn-flip` gate drives the interleaving itself: its `DcnFlipSourceAddress` runs a destroy
of the buffer it programs, at the moment the plane becomes that buffer's.

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

`scanout_requests` is itself counted inside `SetVidPnSourceAddress`, which is why a zero in it says less
than it looks. It separates "the kernel driver refused this candidate" from "nothing asked at this DDI".
It cannot separate "user mode never marked its buffers" from "the request never reached this DDI at all",
and on this driver the second is the usual case: the compositor decides whether a chain is flipped, so a
client whose buffers carry the SCANOUT bit still produces `scanout_requests` 0 when DWM composes it. The
create-time counters answer that half (`type0 creates asked/not by record ...` in the summary): they are
taken in `CreateAllocation`, before any flip, and they say what record each type-0 allocation arrived
with and whether it asked.

`admit_gated` counts the candidates the switch above refused, so a run with no scan-out flip tells a
closed switch from a rule that said no.

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

## The DirectFlip handshake, kernel half (increment 2, 0.7.209.1)

The compositor's user-mode driver may only answer `CheckDirectFlipSupport` TRUE about a surface the
display core can actually read, and it must decide that from the same words and the same arithmetic that
placed the surface. Increment 2 is that shared derivation and the channel that carries the start's answer.
There is no separate `docs/design/direct-flip-handshake.md`: the handshake is the user-mode face of the
admission rule described above, and a second note would be a second place for the same rule to rot.

**One derivation.** `WddmGdiRecordPolicy` (`driver/kmd/gdi_private.h`) turns a type-0 allocation's own
E26R record into its placement. `CreateAllocation` calls it once per call, before it builds anything, and
places every type-0 allocation of that call with the result. `WddmGdiScannable` says whether the display
core can read a surface placed that way - not in the aperture, and `AccessedPhysically`, which is what
asks VidMm for the contiguous physical pages a plane is programmed with.

**Two directions of failure.** The placement derivation is fail-safe for the kernel driver: a call with
no record at all succeeds and yields the plain VRAM placement a standard allocation must get. Read as
"can the display core read this", that same answer is fail-open - it says yes about a record it could not
read. A user-mode caller therefore asks `WddmGdiRecordScannable`, which requires an E26R v3 record of
exactly 64 bytes with the SCANOUT bit and refuses everything else.

**The channel.** `bc250_scanout_caps` is an optional trailer of the `DXGKQAITYPE_UMDRIVERPRIVATE` reply,
after the adapter identity trailer and by the same version-skew rule: 24 bytes at offset 1496, so a
reader must query at least 1520 bytes. It carries one flag - this start will admit a client scan-out flip
- and the POST geometry, which is the only video present source mode `display.c` offers and therefore the
only geometry `Bc250ScanoutAdmit` admits. The shell applies the geometry instead of inferring it from the
compositor's own chain being the desktop's size.

**What the flag means and what closes it.** `EnableDirectFlipHandshake` (REG_DWORD, `Parameters`, default
0) is ANDed at `WddmStart` with `EnableScanoutAdmit` and with every other start-latched fact the flip path
needs: `VidPnFlipEnabled`, mapped MMIO, VRAM, page-aligned VRAM bases, a non-empty POST geometry. The
invariant is that a closed kernel path can never leave the shell agreeing to a flip this driver will
refuse. The dangerous one is `EnableVidPnFlip`: with it closed, `SetVidPnSourceAddress` skips
`DcnFlipSourceAddress`, still publishes and still returns success, so a TRUE answer would stop DWM
composing while HUBP0 keeps the last composed frame - the game runs, every counter says success, and the
screen is frozen. With the switch off nothing is written into the trailer at all, not even a header with
flags 0, so "absent" and "off" are one state on the wire and a reader that forgets the flag cannot act on
a closed switch.

**What the kernel driver does not do with it.** Nothing. The admission reads `scanout_admit.h` and
`AddressAllowed` and never this flag, so a stale or wrong TRUE in user mode cannot widen what may be
programmed. That is deliberate: the handshake buys a copy the OS no longer makes, and nothing else.

**Witnesses.** Three sets, all in `log summary` rather than only in the start-time guard-log lines,
because those lines have a lifetime budget of eight and the compositor's own primaries spend it at boot:
type-0 creates by the record they arrived with, the OS's own `DXGK_SETVIDPNSOURCEADDRESS_FLAGS` per bit,
and the Presents carrying `RedirectedFlip`. The flag bits are read as bitfields and never as transcribed
masks - the WDK header's trailing comments give `0x00000010` twice and are shifted by one bit from
`FlipStereoPreferRight` on, which would turn "the OS did enter independent flip" into its opposite.

## What is still missing

- **The mode list.** `display.c` offers the inherited POST mode only, so a game that asks for 1080p
  cannot mode-set and is scaled and composed. A trial must therefore run at the POST geometry.
- **Multi-plane overlay.** There is no `DxgkDdiCheckMultiPlaneOverlaySupport` and no plane path, so the
  "DWM composes again when a window overlaps" half of M15.14 is handled by the OS falling back to
  composition, not by a driver plane.
- **The desktop UMD's answer.** The compositor's user-mode driver has a `CheckDirectFlipSupport` entry
  since M15.14 increment 1 (`driver/umd/dxvk/ddi-direct-flip.h`), and it answers FALSE for every pair:
  the rule requires both surfaces to carry the SCANOUT bit in their own resource record, and no shell
  writes that bit yet. So DWM will not agree to a direct flip of its surfaces whatever the kernel driver
  admits. A borderless chain's flip is DWM's decision; an exclusive-fullscreen chain is the one shape a
  trial can reach without it, which is why the trial's own client offers `-FlipFullscreen` and why a
  verdict of "the request stopped in user mode" must name DWM as the layer, not the shell.
  Increment 2 publishes the kernel half of the handshake (below) and changes no answer.
- **The composed fallback on the CPU desktop route.** A scan-out surface is VRAM-resident and not CPU
  visible. The record still shares it, so the compositor may open it, but the CPU compositor route
  (llvmpipe, the KMD-swap fallback) composes by reading the surface with the CPU and has no mapping to
  read. On the GPU DWM route - the lab's default since 2026-10-01 - composition reads it with the GPU
  and the fallback holds. M15.14's second sentence ("DWM composes again when a window overlaps") is
  therefore route-dependent for the chains this mode enables, and the overlap case is not yet measured
  on either route. Until it is, scan-out stays a selected mode that the operator turns on.
- **A producer for the E26R scan-out record.** Neither shell writes one yet. The D3D11 shell's
  `convert_runtime_resource` builds a 64-byte v3 record with the access word `PRIMARY` only, never
  `SCANOUT`; the D3D12 shell's `prepare_surface` builds its own 16-byte record (v1 or v2) and the
  compositor's opener takes nothing but v3 at exactly 64 bytes, so a D3D12 swap-chain buffer cannot be
  opened into the compositor's device at all. Both halves are M15.14 increment 2 parts 4 and 5 and neither
  is written; until they are, every arm of a trial reproduces "nothing asked".
- **A producer for BC2A version 3.** No ICD or winsys patch writes `BC250_UMD_A_SCANOUT`. The named user of the BC2A half is the Mesa/RADV winsys
  (`driver/icd/mesa-wddm2-bc250.patch`), where an engine-allocated primary would ask the same question
  through the same words; the kernel side is in and host-tested so that the winsys change is a patch and
  not a contract negotiation. If that route is dropped, the BC2A half goes with it rather than staying
  as a reader with no writer.

Nie wszystko od razu - not everything at once.
