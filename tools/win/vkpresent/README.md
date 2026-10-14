# vkpresent - a Vulkan client that presents frames through the registered ICD and proves every one

Every other Vulkan client in this repository is a compute client: `vkfillcheck`, `vkmembw` and `vksemcheck`
never create a surface, so none of them touches the window-system part of the ICD. The present route of our
Vulkan driver (`radv_wddm2_wsi_route.h` in the Mesa fork: `gdi`, `dxgi`, `dxgi-composition`) therefore had no
client of ours at all, and its lab arms borrowed `vkcube` and Quake II RTX.

This client is ours, it is small, and it answers in one line what those two cannot:

- which ICD **file** the Vulkan loader picked, which is the whole question for the 32-bit half of the machine
- which **route** the ICD then took, measured in the process and not assumed
- whether the pixels of each presented frame are the ones the program asked for.

| File | What it is |
|---|---|
| `vkpresent.c` | The client. One window, one render pass with `loadOp CLEAR`, one present per frame, one readback |
| `build.ps1` | Builds `vkpresent-x64.exe` and `vkpresent-x86.exe` from the NuGet kits, `/W4 /WX` |

## Why both architectures

The release registers a 32-bit Vulkan ICD for the WOW64 half of the machine: the
`HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers` entry and `VulkanDriverNameWow` on the display class key
(`tools/release/driver_package.py`). An x86 client is the only thing that enters that half, and an x64 client
says nothing about it. `build.ps1` builds both from one source and refuses an image that is not the
architecture it was asked for.

## What a frame does

Acquire an image, clear it inside a render pass to a colour that depends on the frame number (so no pipeline
and no shader is involved, and no two frames of a run share a colour), copy one row of the cleared image back
to a host buffer when the surface allows a transfer source, present, wait for the frame's fence, and compare
the row with the colour that was asked for. A driver that drops the clear, presents a stale image or hands out
another frame's image fails the frame instead of passing quietly. `--negative-control` asks for one colour and
expects another: every frame must then fail, which is what proves the comparison is live.

An sRGB surface stores the clear value after the transfer function, so the clear is built from the linear
value of each channel and the comparison stays in the surface's 8-bit space, with one step of slack per
channel. A clear value is always given as red, green, blue, alpha, whatever order the format keeps its
channels in. The readback instead reads the memory of the format, so only that side knows about BGRA. The
first run on the development PC reported every colour reversed (asked `AA327E`, got `7E32AA`) because the
clear was swapped as well.

**The readback needs the surface to allow a transfer source.** Nothing here copies into the image, because the
clear is the render pass's own load operation, so `VK_IMAGE_USAGE_TRANSFER_DST_BIT` is not asked for. On a
foreign ICD a surface without `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` is no failure: the frames are presented and
the line says `readback=unsupported`. On the board it is a failure, because then the run proves nothing about
the frames it presented, which is the only thing it is for. The mask is in the line as `usage=0x...` either
way, so the first lab arm says what our WSI exposes.

## The route is read, never written

The switch belongs to the machine: the `REG_SZ` value `WsiRoute` under `HKLM\SOFTWARE\amdgpu-wddm\Vulkan`,
values `gdi`, `dxgi` and `dxgi-composition`. This client reads it, from the 64-bit view, which is where the
ICD reads it from as well, also in a 32-bit process. It never writes it: the arm that sets the route and the
client that runs under it cannot then disagree about what was asked. `--expect-route` overrides what the line
reports as asked, for an arm that wants to state its own expectation.

The environment switch `AMDGPU_WDDM_VK_WSI` is deliberately unused. The release ICD carries no environment
overrides (BD-167), so an arm built on the registry runs against the release and the lab flavour unchanged.

**The route that actually ran is measured, not assumed.** The DXGI routes bind System32 `dxgi.dll` and
`d3d12.dll` by full path, and the composition route adds `DComp.dll`, while the GDI route loads none of them
(`docs/design/vulkan-wsi-dxgi.md`). The module list of the process after the first present therefore names the
route, and the line says `route=<asked>/<taken>`. An ICD that was asked for `dxgi` and fell back to GDI fails
the run: that fallback is the subject of the route work, not a detail.

That witness speaks for our ICD alone, so it is only read on the board. Another vendor's ICD binds the same
three modules for its own reasons: the first run on the development PC, through the NVIDIA ICD, read
"took dxgi-composition" from a process where our WSI was never loaded. On any device but `0x1002:0x13FE` the
line therefore says `route=<asked>/unknown` and the route decides nothing. `unknown` is a result, never a
value to ask for: the registry and `--expect-route` take the three real routes only.

The program's own folder must hold no `dxgi.dll`, `d3d12.dll` or `d3d12core.dll`. The ICD's module gate refuses
the DXGI route to a process that has an application-local copy of one of them, by design. `vkpresent` says so
in a note instead of leaving the arm to wonder, and `build.ps1` refuses an image that imports any of them
statically.

## The line

```
vkpresent PASS arch=x86 route=dxgi/dxgi source=registry icd=C:\...\wow64\vulkan\vulkan_radeon.dll
  device="AMD Custom GPU 0405 (RADV GFX1013)" api=1.4.0 driver=... format=44 usage=0x9F frames=60
  presented=60 lost=0 readback=ok maxms=16.904 total=1.021s
```

One line, on one line in the real output. `PASS` needs every frame presented, `lost=0`, `readback=ok` (or
`unsupported`, which only a device other than the board may answer), and `route` asked equal to `route` taken
on the board. `--json` writes the same facts as JSON for a runner to read.

The two runs on the development PC, through its own ICD, which prove the present and the comparison of this
program and nothing about our WSI (640 by 360, 60 frames):

```
vkpresent PASS arch=x64 route=gdi/unknown source=default device="NVIDIA GeForce RTX 4090" api=1.4.329
  format=44 usage=0x9F frames=60 presented=60 lost=0 readback=ok maxms=16.724 total=1.018s
vkpresent PASS arch=x86 route=gdi/unknown source=default device="NVIDIA GeForce RTX 4090" api=1.4.329
  format=44 usage=0x9F frames=60 presented=60 lost=0 readback=ok maxms=17.762 total=1.417s
```

Exit: `0` pass, `1` a frame or the route failed, `2` usage, `3` Vulkan or system error, `4` the deadline.

## Running it

```
pwsh tools\win\vkpresent\build.ps1 -Kits <workspace>\toolchain\nuget
vkpresent-x86.exe --frames 60 --json route-dxgi.json
```

`--no-present` enumerates and reports without creating a window, which is what `build.ps1` runs as its
self-test: it is safe on any PC and in a session-0 shell. **A presenting run needs an interactive session**,
and the client refuses session 0 with that reason instead of failing somewhere inside the present. A
shell over SSH on the lab is session 0, where no window is on a monitor and no interactive compositor draws
one, so the client has to be started in the interactive session. `run-vkcube.ps1` of the Vulkan WSI route kit
(`scratch\b24\vk-wsi-dxgi-kit\lab\`) already does exactly that and takes the client as a parameter, in spite
of its name: pass `-Client <path>\vkpresent-x86.exe` and `-ClientArgs`. The kit's steps are also where the
route is set and restored on every exit path.

## What it does not do

It renders nothing but a clear. It is an instrument for the route, the registration and the present, not a
benchmark: a frame rate from this client measures a clear and a present and nothing a game does. For frame
rates the lab uses a game.

PROVENANCE: no third-party code. `evidence/windows/2026-09-25-E31-m10-acceptance/source/color-oracle.c` is a
modified `vkcube` (Khronos, Apache-2.0) that checked colours for the M10 acceptance. It is evidence, frozen
bytes of that experiment, 5300 lines, x64 only, with no route report and no machine-readable line. This client
exists instead of reviving it.
