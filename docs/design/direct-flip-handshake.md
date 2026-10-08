# The DirectFlip handshake: a D3D11_1 front in the desktop router

M15.14 asks that a fullscreen or borderless game is scanned out from its own swap-chain buffer, so that
the compositor does not compose its frames. `docs/design/scanout-admission.md` holds the kernel side of
that: which allocation may be a scan-out candidate, and which address may reach HUBP0. This document
holds the user-mode side, which is the part that was missing.

## Why the question was never asked

The operating system asks one driver entry whether an application's back buffer may take the place of the
compositor's front buffer. That entry is `pfnCheckDirectFlipSupport`. It is declared in
`D3D11_1DDI_DEVICEFUNCS` and in the WDDM2_x device tables only (`d3d10umddi.h:2992`).

The compositor's user-mode driver on this lab is `bc250d3d_zink.dll`, the Mesa `d3d10umd` frontend. That
frontend offers the D3D10.0 DDI alone. Its device table has no slot for the question.

So the answer was never FALSE. There was nothing to call, and the operating system did not ask. Earlier
notes in this repository said that the compositor answered FALSE. That was true of the application shell
`amdgpu_wddm_d3d11.dll`, which the compositor does not load.

## The route

`bc250d3d_router.dll` already decides which user-mode driver each process loads. It is the compositor's
own route, and it already saves and restores the whole `D3D10_2DDI_ADAPTERFUNCS` table around a hosted
open. So the router is where a higher DDI can be offered without a change to Mesa.

The router installs five adapter entries of its own after a successful hosted open. Those five entries
are the front:

| Entry | What the front does |
| --- | --- |
| `pfnGetSupportedVersions` | asks the hosted driver for its list, then appends `D3D11_1_DDI_SUPPORTED` |
| `pfnGetCaps` | answers the D3D11-era caps types itself and forwards the legacy ones |
| `pfnCalcPrivateDeviceSize` | the hosted size, plus the front's own per-device record after it |
| `pfnCreateDevice` | creates the hosted device at the D3D10.0 DDI, then publishes a D3D11_1 table |
| `pfnCloseAdapter` | forgets the adapter, then forwards |

Mesa is not rebuilt and not changed. `bc250d3d_zink.dll` stays the binary that this desktop was measured
with.

### The one cap that turns the front on

`pfnGetCaps` answers `D3D11DDICAPS_3DPIPELINESUPPORT` with pipeline level 10_0 and nothing else. That is
the level the hosted frontend really has. Every entry of the published table that a level 10_0 device
cannot reach refuses the call and writes one line about it.

`D3D11DDICAPS_THREADING` is 0, so there are no command lists and no deferred contexts.
`D3D11_1DDICAPS_D3D11_OPTIONS` is zero in both fields, because the D3D10.0 blend state has no field for
an output-merger logic operation. A caps type from a DDI the front does not offer gets `E_INVALIDARG`. A
zeroed buffer with `S_OK` would publish caps that the front never meant.

### How the handles stay identical

The front's per-device record goes in the device private block that the runtime allocates, after the
hosted driver's own block. `hDrvDevice` is therefore the same pointer for the front and for the hosted
driver.

This is what makes most of the table free. An entry whose argument types are identical in both DDIs is
published as the hosted function itself, with no thunk between the runtime and Mesa. Of the hosted
driver's 101 entries, 75 reach the runtime that way. The front finds its record from `hDevice` through a
small published table of the devices it created.

A per-resource record cannot use the same trick. A resource's private size is a function of the
resource's own shape, so the front cannot recompute the offset later, and a record placed first would
change `hDrvResource` for every forwarded entry. The resource records therefore live in a fixed
open-addressing map that the driver handle keys. Only a surface that could be one side of a DirectFlip
pair is recorded.

### The table, entry by entry

`D3D11_1DDI_DEVICEFUNCS` holds 155 function pointers in this WDK. The declaration counts 157, because two
`D3D10PSGP` members are reserved for system use and are not declared here. The front fills every one of
the 155:

| Group | Count | What the front publishes |
| --- | --- | --- |
| Field copies | 75 | the hosted function itself |
| Thunks and hooks | 32 | a translation of the argument struct, then the hosted entry |
| Refusals | 43 | one body per signature, for entries a level 10_0 device cannot reach |
| Real bodies | 5 | `pfnDiscard`, `pfnAssignDebugBinary`, `pfnCheckDeferredContextHandleSizes`, `pfnClearView` and `pfnCheckDirectFlipSupport` |

A null slot in that table is a call into address zero inside `dwm.exe`. The host gate therefore counts
the slots and fails on a single null one, and `front-adapter.h` asserts the three table sizes at compile
time. The gate also names every one of the 75 copies and every one of the 37 own bodies, and checks that
each copy is the hosted entry it claims: a copy that names its neighbour reads as a pass on a count alone.

The front answers at one interface version. `D3D10DDIARG_CREATEDEVICE` is a union keyed by `Interface`,
so a device created at the D3D10.0 or D3D10.1 DDI holds a smaller function table and a smaller DXGI
table than the front would fill. `CalcPrivateDeviceSize` and `CreateDevice` therefore forward any
interface other than `D3D11_1_DDI_INTERFACE_VERSION` to the hosted driver untouched, and say so once in
the log. Without that guard the front wrote 155 entries into a 101-entry table and 15 into a 7-entry one:
432 and 64 bytes past two buffers the runtime owns, in `dwm.exe`.

`pfnClearView` is the one entry that the D3D10.0 DDI cannot fully express. It clears a view, and the
runtime may give it a list of rectangles. The D3D10.0 clear entries clear a whole view. The front
forwards the whole-view shape and counts a rectangle call under its own name. Zero rectangle calls in the
desktop arm is a pass condition of increment 1. If the compositor does call it with rectangles, that one
entry moves into the Mesa frontend, where a scissored `clear_render_target` can express it.

### The DXGI obligation

At the D3D10.0 DDI the driver fills `DXGI_DDI_BASE_FUNCTIONS`, which holds 7 entries. At D3D11_1 the
runtime hands the driver `DXGI1_2_DDI_BASE_FUNCTIONS`, which holds 15. The union member in
`DXGI_DDI_BASE_ARGS` is the same pointer, so the hosted driver still writes slots 0 to 6 in place, and
the front fills the rest.

Four of the remaining eight belong to the front: `pfnResolveSharedResource`, `pfnBlt1`,
`pfnOfferResources` and `pfnReclaimResources`. Slot 7 arrived at DXGI1_1, so the hosted driver may have
filled it already through the union member. The front keeps a non-null slot and installs its own stub
only where the slot is empty.

`pfnBlt1` is on the desktop's present path, because installing the front is what moves the presentation
blt from `pfnBlt` to it. It never refuses. The older entry carries a destination rectangle, the whole
source subresource and its own `Rotate` field, so an unstretched copy from the source origin is exact and
a rotation needs no clause. Any other source rectangle is named once in the log and forwarded all the
same: wrong pixels in one blt are recoverable, a dead compositor is not.

The last four are the multiplane-overlay entries. The build version does not exclude them: the second
clause of `IS_DXGI_MULTIPLANE_OVERLAY_FUNCTIONS` is `major == 11 && minor > D3D11_1_DDI_MINOR_VERSION_RC`,
which holds at `D3D11_1_DDI_INTERFACE_VERSION` whatever the build version is, and our own DXVK shell
relies on exactly that. What keeps the operating system off them is the kernel driver, which implements no
multiplane-overlay DDI at all, so DXGI has nothing to build an overlay plan from. They answer
`DXGI_ERROR_UNSUPPORTED`, the same refusal our own UMD gives, and write one line. An entry that nobody
should call is the entry that costs a crash when the assumption is wrong.

The DXGI entries carry a `DXGI_DDI_HDEVICE`, which is the runtime's handle and not the device handle the
front publishes, so these bodies have no device record to log through. The front therefore keeps a
process-wide log channel, seeded when an adapter is installed, and every refusal reaches the file the
trial reads.

`pfnCreateDevice` returns the hosted code and not `S_OK`. The Mesa frontend answers
`DXGI_STATUS_NO_REDIRECTION` there, which is a success code that keeps DXGI off the shared-resource
presentation path with the compositor. That is the path this desktop was measured on.

## The rule

`front-direct-flip.h` holds the answer as a pure function of the adapter's published scan-out
capabilities and the two surfaces. A host test drives the production rule and not a copy of it.

The runtime gives the entry two resource handles and nothing else. `hResource1` is the application's
surface, opened into the compositor's device. `hResource2` is the compositor's own surface. The rule
applies these clauses in order, and each clause has a name that a trace prints:

| Refusal | What it means |
| --- | --- |
| `handle` | a null handle, or one surface given twice |
| `gated` | the kernel driver published no DirectFlip admission for this adapter start |
| `record` | one of the two surfaces has no front record |
| `sides` | `hResource1` was not opened, or `hResource2` was not created by this device |
| `primary` | one of them is not a primary of a swap chain |
| `vidpn-source` | they do not name the one video present source this adapter has |
| `client-scannable` | the application's record never asked for scan-out, or its placement cannot be read |
| `compositor-scannable` | the display core cannot read the compositor's buffer where it sits |
| `format` | the storage row is not a `SCANOUT_PRIMARY` row of the shared format table |
| `geometry` | the two differ in width or height |
| `source-geometry` | they differ from the source mode at which the kernel driver admits a flip now |
| `pitch-unknown` | one side's pitch is not known in user mode |
| `pitch` | the two differ in pitch |

The rule assumes no display mode. The geometry of the `source-geometry` clause comes from the scan-out
caps trailer, which the front reads again at every question. In `main`, `display.c` offers the POST mode
alone, so the trailer says 1920x1200 on this lab. A kernel driver that offers more modes writes the
committed mode into the same two fields, and the rule follows without a change. The kernel driver of
the display-modes branch does this (`DisplaySourceWidth` and `DisplaySourceHeight` in `wddm.c`).

### The two sides are not symmetric

Both surfaces must be ones that the display core can read. They earn that differently, and
`driver/kmd/gdi_private.h` holds both derivations:

- The application's surface is shared. A shared record places the allocation in the aperture unless its
  own SCANOUT bit moves it to the local segment. Here the bit is the whole question, and
  `WddmGdiRecordScannable` asks it.
- The compositor's surface is one that the compositor created. It is not shared, so its type-0 placement
  is already VRAM with `AccessedPhysically`, and the display core reads it at the refresh rate already.
  It never asks for scan-out. A rule that demanded the bit of it would refuse every pair that the
  operating system can pass. `WddmGdiCreatedScannable` asks the right question.

Both derivations end in the kernel driver's own placement arithmetic, called from user mode. An answer
given in user mode and the placement given at `DxgkDdiCreateAllocation` cannot disagree.

Neither derivation is `WddmGdiScannable(WddmGdiRecordPolicy(record))`. That composition is fail-open by
its own contract comment. It says yes about a surface whose record could not be read, because the kernel
driver's `CreateAllocation` must still place a standard allocation that carries no record. Read as "can
the display core read this", the same answer is a TRUE about an aperture-resident buffer whose address
`DcnTranslateCardAddress` refuses, after the runtime stopped copying.

### Why a wrong TRUE is worse than a lost optimisation

A flip that fails after `SharedPrimaryTransition` does not fall back. The contract says of that case that
the operating system will not fail back to composition mode, and that presentation will be incorrect
(`ref/ddi-display/d3dkmddi.md:12793`). The screen goes black and nothing composes it again. The rule is
therefore the narrowest one that can be true of both surfaces.

## The switch

`DirectFlipFront`, a REG_DWORD under `HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter`.

| Value | What the route log says | What happens |
| --- | --- | --- |
| 0 | `front=off` | the router that shipped, byte for byte: this feature's bisect switch |
| any other value kind | `front=invalid` | off, and the log says the switch could not be read |
| absent, or any non-zero DWORD | `front=on` | the front is installed over the hosted adapter table. Absent means on from 0.7.213.100-tester.15, where the front became a shipped feature |
| non-zero, front not installed | `front=unavailable` | the D3D10.0 adapter entry, an incomplete hosted table, no free slot, or a route that ended on the CPU UMD |

The value is read at every `OpenAdapter` call, so it is start-latched for `dwm.exe`. A change needs a
Windows restart, which is also what a new compositor process needs. A forced DWM restart breaks WinUI
mouse input on this lab until the next boot.

Per `OpenAdapter` is not per system: a process that opens the adapter after the value changed gets the
new state while `dwm.exe` keeps the old one, so between a write and the restart the desktop and a newly
started application can be on different routes. The route log line of each process says which one it got,
and that is the field to read before comparing two processes in one capture.

The desktop route line of the router gains exactly one column, `front=`, at the end. Every field that the
registered router 674AD261 wrote stays in its place with its spelling, so the deployment kit's parser
still reads the line.

The front writes its own log beside the route log, as `front-<exe>-<pid>.log`. It writes nothing when
`RouteLogDirectory` is absent, so the lab script sets that value as part of the arm.

## What increment 1 did, and what it did not

In increment 1, `pfnCheckDirectFlipSupport` counted the call. It logged both resource identities, both
geometries and the rule's verdict. Then it wrote FALSE.

Nothing else changes. No allocation moves, no placement changes, and the kernel driver is not called. The
kernel driver's `EnableDirectFlipHandshake` stays as the operator left it.

The increment exists to answer one question that no reading of the headers can answer: does the operating
system ask a device whose 3D pipeline level is 10_0? DirectFlip is required of a full-graphics WDDM 1.2
driver, and WDDM 1.2 drivers shipped for feature-level 10_0 and 10_1 parts, so the answer is likely. That
is an inference, not a measurement.

The answer became the rule's answer in increment 2, behind the kernel driver's own caps flag.

## What increment 2 does

`pfnCheckDirectFlipSupport` writes the rule's answer. It writes TRUE when every clause holds, and FALSE
with the name of the first clause that fails.

- **The caps trailer.** The front keeps the runtime's adapter query from `OpenAdapter`. At every question
  it queries a buffer of `BC250_SCANOUT_CAPS_TOTAL` bytes and reads the trailer (`ReadScanoutCaps`). A
  failed query, a kernel driver without the trailer, a closed `EnableDirectFlipHandshake` and a torn
  trailer all read as zero, and the `gated` clause refuses. The runtime asks before the compositor presents
  to a DirectFlip swap chain, after each mode change, and after the compositor re-creates its own swap
  chain (`ref/ddi-display/d3d10umddi.md:8781`). So the query is not a per-frame cost.
- **The compositor's pitch.** The hosted driver allocates the compositor's own buffer later than the
  create, at the first Present or `SetDisplayMode`, and the front does not see that allocation. The hosted
  driver gives every surface that it allocates the same pitch: the row in bytes, rounded up to 256 bytes
  (`Bc250EnsureSurface` in `src/gallium/frontends/d3d10umd/DxgiFns.cpp`, mesa-wddm
  `amdgpu-wddm/b19-hosted-umd` `7eb7861d` and `amdgpu-wddm/hang-recovery-zink` `ea876500`). The front
  applies the same line (`HostedSurfacePitch`). For a 4-byte row it equals the kernel driver's own primary
  pitch, `DcnPrimaryPitch`. A format with no row in the shared table keeps pitch 0, and the rule refuses
  it as `pitch-unknown`.
- **The log line.** Each answer writes one line with `answer=`, `rule=`, the trailer's flag and source
  geometry, and both surfaces with their geometry, pitch, format, record version and access word. The
  install line of the adapter carries the trailer as the adapter open saw it. The file holds at most 256
  answer lines. The device's destroy line holds the counters, which have no limit.
- **The switches.** There is no new switch. `DirectFlipFront = 0` removes the front from the compositor's
  device. `EnableDirectFlipHandshake = 0` in the kernel driver removes the trailer, and the answer is
  FALSE with `rule=gated`. A TRUE needs both switches on, and both are on by default from
  0.7.213.100-tester.15.

A TRUE lets the operating system try the flip. It does not move an allocation. The kernel driver still
applies `Bc250ScanoutAdmit` at every `SetVidPnSourceAddress`. The rule asks the same questions first from
the same words, so after a TRUE only the base address and the segment can still refuse the flip. VidMm
decides both for an allocation that it pinned for scan-out, and `Bc250ScanoutCreateAlignment` asks for
the 4 KiB base at the create.

The client side asks for scan-out only when its own record carries the SCANOUT bit in an E26R v3 record
of 64 bytes. The D3D12 shell writes that record in its scan-out mode (increment 2 of the shell). The
D3D11 shell of increment 2 writes a v3 record without the SCANOUT bit, so a D3D11 client stays composed,
with `rule=client-scannable`.

## What increment 3 changes

Trial 478 (M844) ran The Witcher 3 (D3D12) in three modes. The game flipped independently in exclusive
fullscreen and in borderless at the native 1920x1200. Exclusive fullscreen at 1920x1080 stayed composed,
because the shell's experiment named 1920x1200 as a fixed geometry. The front logged its first 256
answers in the first seconds of the session, so the log had no answer for the later modes. Increment 3
changes these items:

- **The D3D12 shell follows the committed mode.** The scan-out primary is on by default, and the
  experiment `scanout-flip-off` turns it off. The shell reads the trailer again for every primary that it
  creates. The rule that both application shells share (`driver/contract/bc250_scanout_primary.h`)
  compares the chain with the trailer's source geometry, not with a geometry that the operator names.
  The spellings `scanout-flip` and `scanout-flip-1920x1200` still read as on.
- **The front logs by change and by count** (`driver/umd/router/front-flip-log.h`). An answer writes a
  line only when its key differs from the last line of the device. The key is every field of the line
  except the call number and the two handles. The change lines have a budget of 256, and the changes
  past it are counted. A counter per rule, per answer and per `IMMEDIATE` flag counts every call. Every
  30 s while the compositor asks, and once more at the destroy of the device, a
  `check_direct_flip summary` line gives all the counters.
- **The D3D11 shell can ask for scan-out.** `driver/umd/dxvk/scanout-primary.h` sets the SCANOUT bit
  under the same shared rule. It is off by default until a lab arm measures it:
  `AMDGPU_WDDM_D3D11_SCANOUT=1` in the process environment, or `ScanoutPrimary` (REG_DWORD 1) under
  `HKLM\SOFTWARE\amdgpu-wddm\D3D11`, turns it on. The runtime creates some D3D11 window buffers as
  DISPLAYABLE without a primary description (M746). These buffers cannot ask, and the shell logs them
  as `not-primary`.
- **The analyser has a kernel witness.** Win32k marks most game frames of trial 478 with
  `SkipIndependentFlip`, but the kernel events of the same frames record the flip.
  `tools/win/etw/etw-present-mode.py` now reads such a frame as a hardware flip when four clauses hold
  (`tools/win/etw/README.md`).

The rollback ladder for the shell's half: `scanout-flip-off` as the machine value `Experiment` under
`HKLM\SOFTWARE\amdgpu-wddm\D3D12`, then `DirectFlipFront = 0`, then `EnableDirectFlipHandshake = 0`.

## The offline gate

`tools/build/build-umd-router.ps1` compiles the front into the router and into `test-router.exe`, so the
host gate drives the production table fills and the production rule. `tools/build/test-umd-router.ps1`
runs 83 scenarios, each in its own process on a private application hive.

Thirteen of them are M15.14's:

| Scenario | What it settles |
| --- | --- |
| `front-tables` | both table fills, with every slot non-null and the exact split of copies and own bodies |
| `front-rule` | one refusal per clause, the admissible pair, and the pair given the other way round |
| `front-record` | the E26R decoder at 12, 16 and 64 bytes, the LB7A decoder, and the resource map |
| `front-absent`, `front-zero`, `front-wrong-type` | the three states that mean off, and the column each one writes |
| `front-on` | the whole path: versions, caps, device create, the forwards, and the FALSE answer of a start without the trailer |
| `front-answer` | the TRUE answer for the pair the lab passes. Seven negative controls each turn it FALSE under their own clause. |
| `front-log` | 300 more questions over one pair write one line per change of the answer, and the destroy summary counts every answer under its rule |
| `front-d3d10-entry` | the D3D10.0 adapter entry leaves the route unchanged |
| `front-d3d10-interface` | the front forwards a device created at the D3D10.0 interface whole, and writes nothing past either table or past the hosted private block |
| `front-cpu-route` | the kill switch keeps the front out of the path |
| `front-stack` | the front over the real hosted UMD, with its own version list and caps |

`tools/quality/quick.ps1` runs the suites that need nothing from the lab, as the `router-front` gate. The
rest of the router's host gate needs three binaries that are not in this repository, so it runs as
`router-stack` when `BC250_ROUTER_HOSTED_UMD`, `BC250_ROUTER_CPU_UMD` and `BC250_ROUTER_APP_PACKAGE` name
them, and records why it did not when they are absent.

## The lab arms, and the way back

Increment 1 runs two arms, one per invocation, each inside the three-minute bound for a lab trial that
does not start a game:

- Arm 1a, desktop health, with no client. The route line must say `route=hosted` and `front=on`. A File
  Explorer window opens, moves and resizes, because that is the shape that BD-058 needed. The compositor
  must be the same process afterwards. The arm also names the one witness that the lab script cannot
  take: a screenshot through the overlay. A refused front entry in that arm is a finding, not noise: the
  desktop can look right while the runtime drops the call or the frame it belonged to.
- Arm 1b, the question. The borderless client at the POST geometry, under the present-mode provider set,
  with the cursor parked in a corner. The front's log is the first-rank evidence. ETW is the inert
  control, and any deviation there is a defect and not progress.

The rollback ladder, in order. Each rung assumes less than the one above it:

1. `DirectFlipFront = 0` and one Windows restart.
2. `DwmForceCpu = 1` and one Windows restart, which composes the desktop on the CPU UMD.
3. No SSH: `lab-emerg.py ps` writes the same value with no shell, then `lab-emerg.py reboot reboot-now`.
4. The desktop does not come back: `lab-emerg.py usb-boot linux:N`, then `lab-emerg.py reboot reboot-now`.
   The diagnostic stick runs no Windows desktop driver, and the lead fixes the NVMe system from there.
5. Nothing answers at all: cut AC at the plug and boot with the AppRouter allowlist safe boot. An AC cycle
   starts the same route again, so it recovers a hung machine and changes nothing else.

## Open items

- **The cursor.** This part has no hardware pointer, and `driver/kmd/dcn*` holds no cursor register
  knowledge. Under DirectFlip the scanned-out surface is the application's own buffer, so nothing in the
  pipeline can draw a cursor into it. Fullscreen games hide the cursor. Borderless with a visible cursor
  is at risk. No text in `ref/` settles whether the cursor blocks DirectFlip on this hardware.
- **The compositor's pitch.** Increment 2 takes it from the hosted driver's own pitch rule. If a later
  hosted driver changes that rule, `HostedSurfacePitch` must change with it, or the rule refuses every
  pair as `pitch`. That is a lost flip and not a wrong one, because the kernel driver programs the pitch
  of each flip from the flipped allocation.
- **The D3D11 client.** The D3D11 shell does not set the SCANOUT bit yet, so a D3D11 game stays composed.
- **10-bit and HDR.** The kernel's scan-out format table admits 8-bit `SCANOUT_PRIMARY` rows only. The
  rule compares the storage row, so it refuses a 10-bit pair honestly until that table is wider.
- **The interval bound.** `FlipImmediateMmIo` and `FlipInterval` are not declared, so a chain at present
  interval 0 cannot reach independent flip on this driver. `DdiPresentForIFlip` is not declared either,
  and it is the only safe refusal point for a present that might become an independent flip.

Nie wszystko od razu - not everything at once.
