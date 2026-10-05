# vkfillcheck

A content gate for the three Vulkan buffer transfer commands: `vkCmdFillBuffer`, `vkCmdUpdateBuffer` and
`vkCmdCopyBuffer`. One C file over core Vulkan 1.1, no shaders of its own, no window. It runs from an SSH
session 0 shell.

RADV runs all three commands as its own compute meta shaders. A meta shader built from a wrong key writes
the wrong bytes, or writes outside the range the application asked for. Nothing reports this. The commands
return no status. The queue reports no error. A store through a wrong address is a GPU fault only when that
address is unmapped, so the same defect can bugcheck one machine and silently corrupt another. Only a byte
comparison finds it, and that is what this client does.

## Build

```
pwsh tools\win\vkfillcheck\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
```

Output: `<BC250_ROOT>\scratch\build\vkfillcheck\vkfillcheck.exe`. The build compiles with `/W4 /WX`, keeps the
previous executable under `retained\` by its hash, and prints the SHA-256 that a lab runner pins. It then
runs three checks: `--help`, a quick self-test on this PC when a Vulkan device of any vendor is present, and
the negative control. `-SkipSelfTest` leaves out the last two. `-VulkanInclude` points at the Vulkan headers
(default: the headers in `scratch\m15\vkd3d\khronos\Vulkan-Headers\include`).

## What one case does

1. It writes a sentinel over a window of the destination. The window is `dstOffset + size + 256` bytes.
2. It records one transfer command into the range `[dstOffset, dstOffset + size)`.
3. It submits the command buffer and waits for the fence.
4. It reads the window back and compares every byte with the expected image: the command's data inside the
   range, the untouched sentinel outside it.

The `dstOffset` bytes before the range and the 256 bytes after it are guard bytes. A store that leaves the
range fails the case, even when every byte inside the range is correct. RADV's current buffer meta shader has
no destination clamp: its bounds come only from the thread count of the dispatch, so an off-by-one in that
count writes past the end with no other symptom.

Each case also checks that its own expected image differs from its own sentinel image. A driver that drops
the command and does nothing therefore cannot pass. Each case carries its own salt, so the data that the
previous case left behind cannot pass either.

## The matrix

| Dimension | Values | What it drives |
|---|---|---|
| Operation | `fill`, `update`, `copy`, `copy-parts` | The three commands, and a copy split into 3 regions |
| Size | 4, 12, 64, 252, 4096, 65536, 1048580 | 4 is one dword. 12, 252 and 1048580 are not a multiple of the shader's dwords per thread. 65536 is the largest `vkCmdUpdateBuffer`. 1048580 needs many workgroups |
| Destination offset | 0, 4, 252 | The destination alignment, and a range that starts inside a 256-byte block |
| Source offset | 0, 1, 2, 3 (copies only) | The source alignment, including the three unaligned cases |
| Destination memory | `host`, `local` | A `HOST_VISIBLE \| HOST_COHERENT` type, and a `DEVICE_LOCAL` type |

`copy-parts` sends 3 regions in one `vkCmdCopyBuffer`. The regions together cover the range, they overlap
nowhere in the destination, each one reads from a different place in the source, and they go in the command
out of destination order.

A `host` destination is the oracle: the sentinel and the readback are plain CPU writes and reads, so the case
depends on nothing but the command under test. A `local` destination needs `vkCmdCopyBuffer` for both, so
each `local` case reads its sentinel back before the operation runs and reports a failure there as
`stage prefill`. The three cases where `size` is 1048580 and the operation is `update` are skipped:
`vkCmdUpdateBuffer` takes at most 65536 bytes.

The full matrix is 396 cases. It takes 0.4 s on an RTX 4090. `--quick` cuts it to 88 cases for a smoke run.

## Choosing the ICD

Three ways, in the order of how much they bypass:

| Option | What it does |
|---|---|
| `--icd <absolute DLL>` | Loads that DLL and calls `vk_icdGetInstanceProcAddr` in it. No Vulkan loader, no JSON manifest, no registry entry. |
| `--manifest <JSON>` | Sets `VK_DRIVER_FILES` and `VK_ICD_FILENAMES`, then uses `vulkan-1.dll`. |
| neither | Uses `vulkan-1.dll` and whatever the loader selects. |

`--icd` is the lab path, for two reasons. `amdgpu_wddm_radv.dll` is a private ICD: the D3D11 and D3D12 shells
load it by absolute path with `LoadLibraryExW` and `GetProcAddress(dll, "vk_icdGetInstanceProcAddr")`
(`driver/umd/dxvk/engine-modules.cpp`, `driver/umd/d3d12/adapter-caps.cpp`), and it has no manifest and no
registry entry. `--icd` loads it the same way, with the same search flags, so the gate tests the file the
shells load. The second reason is elevation: the Vulkan loader ignores `VK_DRIVER_FILES` and
`VK_ICD_FILENAMES` in an elevated process (facts in `docs/research/m7-full-wddm-miniport.md`), and every SSH
session on unit A is elevated, so `--manifest` cannot steer such a session.

The client prints every loaded module that exports `vk_icdGetInstanceProcAddr`, with the SHA-256 of its file.
That line is the route witness: it says which driver the run exercised, whoever chose it.

## On the lab

Copy the executable over and run it against the private D3D12 ICD:

```
python tools\win\target.py push <BC250_ROOT>\scratch\build\vkfillcheck\vkfillcheck.exe --to C:\BC250\vkfillcheck
python tools\win\target.py run "C:\BC250\vkfillcheck\vkfillcheck.exe --icd 'C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_radv.dll'"
```

The D3D11 ICD is the same build in the release kit, at
`C:\Program Files\amdgpu-wddm\d3d11\amdgpu_wddm_radv.dll`. The registered system ICD is a different file:
`C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll`, with `radeon_icd.json` next to it. The loader finds
that one from the registry, so a bare `vkfillcheck.exe` tests it. To test a candidate before an install, give
`--icd` the candidate's own path.

A failing gate can mean a wild GPU write, so treat a first run on a new ICD as a trial that can bugcheck the
machine: keep the run bounded, and read the Tctl and plug rules in `CLAUDE.md` first.

## Output and exit codes

One line per case, then a summary and a result line. A failing line names the stage, the first byte that
differs, which zone it is in, the two values, and how many bytes differ in each of the three zones:

```
FAIL fill       size      12 off   0 src 0 dst host   stage op, first diff at byte 0 (range, +0 from
dstOffset): got 0xC6 want 0x63; bad bytes pre 0 range 4 post 4
```

`--only-fail` prints the failing cases alone. `--negative-control` records every operation 4 bytes late, so
every case must fail, and about half of the failing bytes must land in the guard after the range. A case that
passes under the negative control means the comparison itself is broken, and the client says so and exits 3.

| Code | Meaning |
|---|---|
| 0 | Every case passed |
| 1 | A case failed |
| 2 | Bad arguments |
| 3 | Vulkan or system error, including a fence that did not signal and a broken negative control |
| 4 | The deadline ended the run (`--deadline`, default 25 s, at most 170 s) |

A watchdog thread ends the process 2 s after the deadline, so a driver that never returns from a wait cannot
hold the gate open.

## Options

```
vkfillcheck [--icd DLL | --manifest JSON] [--device N] [--any-device] [--list-devices]
            [--placement host|local|both] [--queue-family N] [--quick] [--only-fail]
            [--negative-control] [--deadline SEC] [--fence-timeout-ms N] [--help]
```

The client looks for the BC-250 (vendor `0x1002`, device `0x13FE`) and refuses any other device.
`--any-device` accepts one, which is how the self-test runs on a development PC. The default queue family is
the first one with `VK_QUEUE_GRAPHICS_BIT`, because that is where applications issue their transfers and
where RADV runs its meta shaders.

## Reference results

Development PC, RTX 4090, driver `0x94D84000`:

| Arguments | Result |
|---|---|
| `--any-device` (the loader picks the ICD) | 390 PASS, 0 FAIL, 6 SKIP, 0.4 s, exit 0 |
| `--icd <the NVIDIA ICD DLL> --any-device` | ICD interface 7, 390 PASS, 0 FAIL, 6 SKIP, 0.6 s, exit 0 |
| `--any-device --negative-control` | 0 PASS, 390 FAIL, exit 1 |
| no option, with no BC-250 present | refused, exit 3 |

The second row is the positive control of the direct load path: a known good driver passes every case when
the client loads it the way the lab needs, without the Vulkan loader.

Ufaj, ale sprawdzaj. (Trust, but check: the driver reports success either way.)
