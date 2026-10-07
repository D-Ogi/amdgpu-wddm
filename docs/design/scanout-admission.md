# Scan-out admission: which allocation the display pipeline may read

M15.14, KMD 0.7.207.1. This note says what changed, why the change is shaped this way, and what a reader
must check before trusting it. The rule itself is `driver/kmd/scanout_admit.h`. Its host test is
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
  which describes itself. The bit implies `PRIMARY` and excludes `CPU_READ`. The reader refuses a record
  that breaks either rule outright. The bit also moves the allocation out of the shared aperture into the local
  segment, because the aperture is the one placement a scanned-out surface can never have.

An allocation that asked for nothing behaves exactly as before. dxgkrnl's shared primary is admitted by
the same four checks it always was, with no segment, address or pitch requirement added to it. That is
the clause which keeps the lab's desktop on the screen, and the host test asserts it.

## What a scan-out candidate must satisfy

In the order the function applies them:

| Status | Rule |
| --- | --- |
| `format` | the format is a `SCANOUT_PRIMARY` row of `driver/contract/amdgpu_wddm_surface_format.h`. `driver/kmd/plane_format.h` has an encoding for it at the same bytes per pixel. The rows are BGRA8, X8, RGBA8 and RGB10A2 (the last two from 0.7.216.20) |
| `geometry` | the width and height are the POST mode's. Only one VidPN source mode exists (`display.c`) |
| `pitch` | `DcnLinearSurfaceBytes` gives the surface a row layout at the row's bytes per pixel. This also refuses a pitch that is not a whole number of pixels, which is what keeps `HUBPREQ0_DCSURF_SURFACE_PITCH` (pitch / bytes per pixel - 1) exact |
| `size` | the rows fit in the allocation |
| `alignment` | the address is 4 KiB aligned |
| `segment` | `PrimarySegment` is the local segment, the only one whose descriptor carries `Flags.DirectFlip` |

The last two apply to a scan-out candidate alone.

The table above is the order the function tests the rules in. The status numbers in
`driver/kmd/scanout_admit.h` are in a different order, segment before alignment, and the driver's
summary line `wddm summary: scan-out refusals format/geom/pitch/size/segment/align/gated` follows
the header. `geom` and `align` are short there because the line holds 159 characters, and the
header gives both names in full.

## Plane pixel formats (0.7.216.20)

Up to 0.7.216.18 only BGRA8 and X8 had `SCANOUT_PRIMARY`, because the driver did not change the
plane's pixel format. The firmware leaves the plane in ARGB8888, and every flip used that format. In lab
session 458 The Witcher 3 presented from an `R8G8B8A8_UNORM` swap chain at 1920x1200. The router's
front answered `answer=0 rule=primary`, and DWM composed every frame.

From 0.7.216.20 the driver programs the plane's pixel format at each flip. The surface format table is
still the single source of truth. A row with `SCANOUT_PRIMARY` must have an encoding in
`driver/kmd/plane_format.h`, and the `scanout-admit` host test holds the two together.

| Table row | DXGI swap chain format | HUBP `SURFACE_PIXEL_FORMAT` | CNVC `CNVC_SURFACE_PIXEL_FORMAT` | Crossbar `CB_B`, `CR_R` |
| --- | --- | --- | --- | --- |
| BGRA8 | `B8G8R8A8_UNORM` (87), `_SRGB` (91) | 8 (ARGB8888) | 8 | 2, 3 |
| X8 | none (the GDI desktop) | 8 | 8 | 2, 3 |
| RGBA8 | `R8G8B8A8_UNORM` (28), `_SRGB` (29) | 8 | 8 | 3, 2 |
| RGB10A2 | `R10G10B10A2_UNORM` (24) | 10 (ABGR2101010) | 10 | 3, 2 |
| RGBA16F | `R16G16B16A16_FLOAT` (10) | not admitted | not admitted | not admitted |

The registers are `HUBP0_DCSURF_SURFACE_CONFIG` (field `SURFACE_PIXEL_FORMAT`), `HUBPRET0_HUBPRET_CONTROL`
(fields `CROSSBAR_SRC_CB_B` and `CROSSBAR_SRC_CR_R`) and `CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT`, from
`third_party/linux-amdgpu/dcn_2_0_1_offset.h` and `dcn_2_0_1_sh_mask.h`. The values come from
`hubp1_program_pixel_format` (Linux v6.18 `dcn10_hubp.c:236-300`, which `dcn201_hubp.c` calls) and
`dpp201_cnv_setup` (`dcn201_dpp.c:44-176`). The firmware's own values (`SURFACE_PIXEL_FORMAT` 8,
`HUBPRET_CONTROL` 0x00E40000) are in E21 and E22. The driver reads the three registers once at start.
It writes back the firmware's values with only the named fields replaced. It does not write any other
field of these registers.

The write order is in `DcnFlipWriteSequence` (`driver/kmd/dcn.c`). The CNVC, crossbar and surface format
writes come before the pitch and the address, inside the same `OTG_MASTER_UPDATE_LOCK` window. The driver
clears `SURFACE_FLIP_TYPE`, so the format, the swizzle and the address all latch at the same VUPDATE. A
format change is a full update in Linux (`dc.c:2672`), and Linux also writes it under the pipe lock
(`dcn20_hwseq.c:1747` and `:1843`).

The driver restores ARGB8888 on these paths:

- the flip back to the desktop's primary.
- the restore of the firmware's surface at stop and at D3 (`DcnRestorePostDisplay`).
- a flip of an allocation that has no plane encoding. The driver refuses this flip with
  `STATUS_NOT_SUPPORTED` and does not change the plane.

The retained power pause sets the current format to "unknown", so the next flip writes the format again.
The CPU mapping of the scan-out surface (`DcnScanoutMapping`) is available only while the plane uses
ARGB8888.

### The capability flag

The scan-out caps trailer gets the flag `BC250_SCANOUT_CAPS_PLANE_FORMATS` (0x2). The driver sets it only
with `BC250_SCANOUT_CAPS_DIRECT_FLIP`, and only when the start read the firmware's format registers and
found ARGB8888. The table gets the policy bit `AMDGPU_WDDM_SURFACE_FIRMWARE_PLANE` for BGRA8 and X8. A
user-mode reader admits a row without that bit only when the trailer has the flag
(`bc250_scanout_format_admitted` in `driver/contract/bc250_scanout_caps.h`). A new user-mode driver on an
older kernel driver therefore keeps RGBA8 and RGB10A2 composed. This is necessary because a refused flip
comes after `SharedPrimaryTransition`, and then the output stays black.

The router's front admits a pair whose two rows differ, for example an RGBA8 client over DWM's BGRA8
buffer. Both rows must pass `bc250_scanout_format_admitted` and must have the same bytes per pixel. The
DDI asks for compatible swizzle formats, and it refuses an `IMMEDIATE` flip only for a swizzle that can
change at VSync alone (`ref/ddi-display/d3d10umddi.md:8833-8838`). Here the address also changes at
VSync alone, so the two always change in the same frame, and the front keeps `IMMEDIATE` admitted.

The operator switch is `EnableScanoutPlaneFormats` (`REG_DWORD`, `Parameters` key, absent means 1). The
value 0 makes the driver skip the start's register read, so the flag stays clear and every flip uses
ARGB8888. This is the bisect switch of the release train.

### Why FP16 is not admitted

The plane has FP16 surface formats: `SURFACE_PIXEL_FORMAT` 24 (both FP16 orders, `dcn10_hubp.c`) in HUBP and
`CNVC_SURFACE_PIXEL_FORMAT` 25 (ABGR16161616F) in `dpp201_cnv_setup`. Two parts are missing:

1. **Request and deadline settings.** A 64-bit surface is `dm_444_64` in DML (`dcn20_fpu.c:1703`). It
   needs other DLG/TTU values, request sizes and watermarks than the 32-bit surface that the firmware
   programmed (`dm_444_32`, `dcn20_fpu.c:1716`, `dcn20_hubbub.c:148-151`). This driver does not run DML.
   With the firmware's 32-bit values the plane gets twice the bytes a line, and an underflow is possible.
2. **The transfer function.** scRGB values are linear and can be outside 0 to 1. The plane needs a
   regamma to sRGB in `MPCC_OGAM` and a clamp. The driver does not program the output gamma.

The RGBA16F row gets `SCANOUT_PRIMARY` only when `plane_format.h` gets its encoding together with these two
parts.

### Colour space

`DXGKARG_SETVIDPNSOURCEADDRESS` does not carry a colour space. Only the multi-plane overlay DDI carries one
(`ColorSpaceType` in `DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES3`, `ref/ddi-display/d3dkmddi.md:23606`). The plane
therefore applies the desktop's transfer function to RGB10A2. An HDR10 (ST 2084) chain gets no PQ
curve. This is a named follow-up for the multi-plane overlay work (`kmd/display-modes`).

### Alpha

The firmware sets `MPCC_ALPHA_BLND_MODE`, and the driver does not change it. The start logs
`MPCC_CONTROL`, `FORMAT_CONTROL` and `ALPHA_2BIT_LUT`. If the mode is per-pixel alpha, an application whose
alpha channel is not 1.0 can get a darker image. A swap chain with `DXGI_ALPHA_MODE_IGNORE` does not
guarantee alpha 1.0 in the buffer. The first lab arm reads the logged mode.

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
  cannot detect that and the operator cannot either.
- failing the call tells the truth. `d3dkmddi` permits any `NTSTATUS` here, dxgkrnl's reaction is its
  own, and the counters carry the diagnosis (`admit_segment`, `admit_alignment`, `admit_geometry`).

What follows from it: the experiment must not be left on for a chain the rule cannot admit. The D3D12
shell therefore asks for scan-out only when the operator named the mode and the chain matches it
exactly, and the refusal path has its own guard-log budget so the one line that says which clause
refused the surface is not spent at the frame rate. The 0x116 exposure of a sustained refusal stream is
not measured. Until it is, the operator ends a trial that reports `admit_*` refusals rather than extends it.

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
allocation is a candidate. `AddressAllowed` decides which address may be written. A wrong address in
HUBP0 is not recoverable on this part - there is no working GPU reset (facts M53) - so the second
refusal stays behind the first on purpose, and neither is allowed to assume the other.

## The DirectFlip handshake, kernel half (increment 2, 0.7.213.1)

The half below was written as revision 209 on `kmd209-iflip2`; train b20 merged it into the lineage that
ships, so 0.7.213.1 is the first driver that has it and no 0.7.209 was ever released.

The compositor's user-mode driver may only answer `CheckDirectFlipSupport` TRUE about a surface the
display core can actually read, and it must decide that from the same words and the same arithmetic that
placed the surface. Increment 2 is that shared derivation and the channel that carries the start's answer.
The user-mode half of the same handshake is `docs/design/direct-flip-handshake.md`. That note names the
driver which carries the entry, the rule behind the answer and the registry value which installs it.
M15.14 increment 1 installed the entry and wrote FALSE. Increment 2 reads the channel below at every
question and writes the rule's answer, TRUE when every clause holds. The two notes describe one rule from two sides, and the user-mode rule may answer TRUE only about a
surface this section admits.

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
1 from 0.7.213, and 0 is its bisect switch) is ANDed at `WddmStart` with `EnableScanoutAdmit` and with every other start-latched fact the flip path
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
- **FP16 and HDR scan-out.** See [Why FP16 is not admitted](#why-fp16-is-not-admitted) and
  [Colour space](#colour-space).
- **Multi-plane overlay.** There is no `DxgkDdiCheckMultiPlaneOverlaySupport` and no plane path, so the
  "DWM composes again when a window overlaps" half of M15.14 is handled by the OS falling back to
  composition, not by a driver plane.
- **The desktop UMD's answer.** DWM's own user-mode driver is `bc250d3d_zink.dll`, the hosted Mesa
  `d3d10umd` frontend at the D3D10.0 DDI, and `pfnCheckDirectFlipSupport` is absent from that table. The
  answer is therefore not FALSE. The operating system has no entry to call, and the D3D11 shell's own
  entry (`driver/umd/dxvk/ddi-direct-flip.h`) is the application's driver, which the compositor never
  loads. M15.14 increment 1 supplies the missing entry: a `D3D11_1DDI_DEVICEFUNCS` front inside
  `bc250d3d_router.dll` which hosts the same Mesa device byte for byte
  (`driver/umd/router/front-device.cpp`, value `DirectFlipFront` under the router key, absent means on from
  0.7.213.100-tester.15 and 0 is its bisect switch).
  In increment 1 that front counted the call, logged both resource records and wrote FALSE, because it
  read no caps trailer and the rule's first clause answered `gated`
  (`driver/umd/router/front-direct-flip.h`). So DWM did not agree to a direct flip of its surfaces
  whatever the kernel driver admitted. A borderless chain's flip and a fullscreen chain's flip are the same
  decision and DWM makes both, so a verdict of "the request stopped in user mode" must name DWM as the
  layer, not the client's shell. Increment 2 reads the kernel half below and is the first revision which
  answers TRUE. The route is in `docs/design/direct-flip-handshake.md`.
- **The composed fallback on the CPU desktop route.** A scan-out surface is VRAM-resident and not CPU
  visible. The record still shares it, so the compositor may open it, but the CPU compositor route
  (llvmpipe, the KMD-swap fallback) composes by reading the surface with the CPU and has no mapping to
  read. On the GPU DWM route - the lab's default since 2026-10-01 - composition reads it with the GPU
  and the fallback holds. M15.14's second sentence ("DWM composes again when a window overlaps") is
  therefore route-dependent for the chains this mode enables, and the overlap case is not yet measured
  on either route. Until it is, scan-out stays a selected mode that the operator turns on.
- **A producer for the E26R scan-out record.** Neither shell writes one the compositor's opener takes. The
  D3D11 shell's `convert_runtime_resource` builds a 64-byte v3 record with the access word `PRIMARY` only,
  never `SCANOUT` (`driver/umd/dxvk/ddi-resource.cpp:138`). The D3D12 shell's `prepare_surface` does set
  `SCANOUT`, behind the operator's experiment value, in its own 16-byte record (v1 or v2,
  `driver/umd/d3d12/allocation-request.h:100-136`). The compositor's opener takes nothing but v3 at
  exactly 64 bytes, so a D3D12 swap-chain buffer cannot reach the compositor's device at all. Both halves
  are M15.14 increment 2 parts 4 and 5 and nobody wrote either one. Until somebody does, every arm of a
  trial reproduces "nothing asked".
- **A producer for BC2A version 3.** Nothing writes `BC250_UMD_A_SCANOUT`: no shell, no ICD and no winsys
  patch. Both shells ask through the E26R record of the bullet above, which is the type-0 path. The named
  user of the BC2A half is the Mesa/RADV winsys (`driver/icd/mesa-wddm2-bc250.patch`), where an
  engine-allocated primary would ask the same question through the same words. The kernel side is in and
  host-tested, so the winsys change stays a patch and not a contract negotiation. If somebody drops that
  route, the BC2A half goes with it rather than staying as a reader with no writer.

Nie wszystko od razu - not everything at once.
