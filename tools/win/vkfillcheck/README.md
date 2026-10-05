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
pwsh tools\win\vkfillcheck\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Arch x64
```

Output: `<BC250_ROOT>\scratch\build\vkfillcheck\vkfillcheck-x64.exe` and `vkfillcheck-x86.exe`. Both are
built by default, because the ICD is a pair: we build `amdgpu_wddm_radv.dll` for x64 and for x86, the x86 one
serves the WOW64 clients, and an x86 DLL cannot be loaded into a 64-bit process. A gate that ran only the x64
program would cover half of a release. `-Arch x64` or `-Arch x86` builds one of them.

The build compiles with `/W4 /WX`, keeps the previous executables under `retained\` by their hash, checks
that each image is the architecture it asked for, and prints the SHA-256 that a lab runner pins. It then
runs, for each program, `--help`, a usage check, a quick self-test when this PC offers that program a Vulkan
device of any vendor, and the negative control. `-SkipSelfTest` leaves out the last two. `-VulkanInclude`
points at the Vulkan headers (default: the headers in
`scratch\m15\vkd3d\khronos\Vulkan-Headers\include`).

## What one case does

1. It writes a sentinel over a window of the destination. The window is `dstOffset + size + 256` bytes,
   or the whole buffer for a `VK_WHOLE_SIZE` fill.
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

RADV builds the shader key from the destination address, not from the Vulkan offset, so this client drives
the key through the offsets and sizes it asks for. It does not predict the key. These are the key fields and
the way a case reaches each one (`ac_prepare_cs_clear_copy_buffer`):

| Key field | How a case reaches it |
|---|---|
| `dwords_per_thread` | 2 up to 64 KiB, 4 above it, so sizes sit on both sides of that step |
| `dst_align_offset` | `dst_va % (dwords_per_thread * 4)`: 0 to 7 up to 64 KiB, 0 to 15 above it |
| `dst_last_thread_bytes` | `(dst_align_offset + size) % (dwords_per_thread * 4)`, 0 to 15 |
| `dst_single_thread_unaligned` | One thread, an unaligned start and a partial end: 1 to 7 bytes, unaligned |
| `has_start_thread` | Set unless the thread-aligned destination is 256-byte aligned |
| `src_align_offset` | `src_offset % 4`, so the copies use source offsets 0 to 3 |
| `is_clear`, `clear_value_size_is_4` | Every fill: `vkCmdFillBuffer` always carries a 4-byte value |
| the `VK_WHOLE_SIZE` rounding | `fillSize = vk_buffer_range(...) & ~3ull`, which only `VK_WHOLE_SIZE` reaches |

`src_scalarize_for_sparse` and `clear_value_size_is_12` are the two key fields left out: the first needs a
sparse buffer, and Vulkan has no 12-byte buffer clear value.

Only `vkCmdCopyBuffer` can ask for an unaligned destination. `vkCmdFillBuffer` and `vkCmdUpdateBuffer` need a
dword-aligned offset and a dword-multiple size, while a copy has no alignment rule at all
(`radv_fill_memory_internal` asserts `!(dst_va & 3)`, `radv_copy_memory` asserts nothing). The twelve
destination residues that are not a multiple of four are therefore reached with copies, in the `dst-align`
and `single-thread` groups.

The cases come in four groups:

| Group | Operations | Sizes | Destination offsets | Third axis |
|---|---|---|---|---|
| `core` | `fill`, `update`, `copy`, `copy-parts` | 4, 12, 64, 252, 4096, 65536, 65540, 1048580 | 0, 4, 252, 256 | source offset 0 to 3 |
| `dst-align` | `copy` | 12, 65540 | 0 to 15 | source offset 0, 3 |
| `single-thread` | `copy` | 1, 2, 3, 5, 7 | 1, 2, 3, 5, 6, 7 | source offset 0, 1 |
| `whole-size` | `fill` with `VK_WHOLE_SIZE` | 12, 4100 | 0, 4 | tail 1, 2, 3 |

In `core`, 4 is one dword, 12 and 252 are not a multiple of the shader's dwords per thread, 65536 is the
largest `vkCmdUpdateBuffer`, 65540 is the first size that takes four dwords per thread, and 1048580 needs
many workgroups. Offsets 0 and 256 leave `has_start_thread` clear, 4 and 252 set it.

`copy-parts` sends 3 regions in one `vkCmdCopyBuffer`. The regions together cover the range, they overlap
nowhere in the destination, each one reads from a different place in the source, and they go in the command
out of destination order.

A `whole-size` case gets a buffer of its own, `dstOffset + size + 256 + tail` bytes long, and fills it with
`VK_WHOLE_SIZE`. The driver must fill the rest of that buffer rounded down to a dword and leave the last
`tail` bytes alone, so those bytes are the check. The client compares every byte of such a buffer.

Every group runs against both destination placements. A `host` destination is the oracle: the sentinel and
the readback are plain CPU writes and reads, so the case depends on nothing but the command under test. A
`local` destination needs `vkCmdCopyBuffer` for both, so each `local` case reads its sentinel back before the
operation runs and reports a failure there as `stage prefill`. The client skips the 16 cases where the
operation is `update` and the size is over 65536: `vkCmdUpdateBuffer` takes at most that many bytes.

The full matrix is 880 cases, of which 864 run. It takes 0.6 s on an RTX 4090. `--quick` keeps the first two
values of each axis of each group, 88 cases, for a smoke run.

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
python tools\win\target.py push <BC250_ROOT>\scratch\build\vkfillcheck\vkfillcheck-x64.exe --to C:\BC250\vkfillcheck
python tools\win\target.py push <BC250_ROOT>\scratch\build\vkfillcheck\vkfillcheck-x86.exe --to C:\BC250\vkfillcheck
python tools\win\target.py run "C:\BC250\vkfillcheck\vkfillcheck-x64.exe --icd 'C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_radv.dll'"
python tools\win\target.py run "C:\BC250\vkfillcheck\vkfillcheck-x86.exe --icd 'C:\Program Files\amdgpu-wddm\wow64\d3d11\amdgpu_wddm_radv.dll'"
```

Both runs belong to one gate. The second one tests the x86 ICD that the WOW64 clients load, and only the x86
program can load it: an x86 DLL does not go into a 64-bit process. Take the `icd module` line of each output
as the witness of which file the run exercised.

The installed files, as `tools\release\release-sources.json` lays them out under
`C:\Program Files\amdgpu-wddm`:

| File | Who loads it | Which program tests it |
|---|---|---|
| `d3d12\amdgpu_wddm_radv.dll` | the D3D12 shell | `vkfillcheck-x64 --icd` |
| `d3d11\amdgpu_wddm_radv.dll` | the D3D11 shell | `vkfillcheck-x64 --icd` |
| `desktop\amdgpu_wddm_radv.dll` | the desktop route | `vkfillcheck-x64 --icd` |
| `vulkan\vulkan_radeon.dll` | the Vulkan loader, from the registry | a bare `vkfillcheck-x64` |
| `wow64\d3d11\amdgpu_wddm_radv.dll` | the 32-bit D3D11 shell | `vkfillcheck-x86 --icd` |
| `wow64\vulkan\vulkan_radeon.dll` | the Vulkan loader in a 32-bit process | a bare `vkfillcheck-x86` |

The x64 copies are one build and the two x86 copies are another, so two `--icd` runs cover the pair. To test
a candidate before any install, give `--icd` the candidate's own path.

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
| 4 | The deadline ended the run (`--deadline`, default 60 s, at most 170 s) |

A watchdog thread ends the process 2 s after the deadline, so a driver that never returns from a wait cannot
hold the gate open.

## Options

```
vkfillcheck-x64 [--icd DLL | --manifest JSON] [--device N] [--any-device] [--list-devices]
                [--placement host|local|both] [--queue-family N] [--quick] [--only-fail]
                [--negative-control] [--deadline SEC] [--fence-timeout-ms N] [--help]
```

`vkfillcheck-x86` takes the same options.

The client looks for the BC-250 (vendor `0x1002`, device `0x13FE`) and refuses any other device.
`--any-device` accepts one, which is how the self-test runs on a development PC. The default queue family is
the first one with `VK_QUEUE_GRAPHICS_BIT`, because that is where applications issue their transfers and
where RADV runs its meta shaders.

## Reference results

Development PC, RTX 4090, driver `0x94D84000`, 2026-10-05:

| Program and arguments | Result |
|---|---|
| `vkfillcheck-x64 --any-device` (the loader picks the ICD) | 864 PASS, 0 FAIL, 16 SKIP, 0.6 s, exit 0 |
| `vkfillcheck-x86 --any-device` | 864 PASS, 0 FAIL, 16 SKIP, 0.8 s, exit 0 |
| `vkfillcheck-x64 --icd <the 64-bit NVIDIA ICD> --any-device` | ICD interface 7, 864 PASS, 0 FAIL, 16 SKIP, 0.6 s, exit 0 |
| `vkfillcheck-x86 --icd <the 32-bit NVIDIA ICD> --any-device` | 864 PASS, 0 FAIL, 16 SKIP, 0.8 s, exit 0 |
| `--any-device --negative-control`, both programs | 0 PASS, 864 FAIL, exit 1 |
| no option, with no BC-250 present | refused, exit 3 |

The two direct-load rows are the positive control of the lab path, for both architectures: a known good
driver passes every case when the client loads its DLL the way the lab needs, without the Vulkan loader. The
negative control rows prove that the comparison can fail, guard bytes included.

Unit A, 2026-10-05, release 0.7.207.100-tester.12 (evidence `evidence/windows/2026-10-05-E55-vkfillcheck-lab`):

| Program and ICD (`--icd`) | Result |
|---|---|
| x64, deployed `d3d12\amdgpu_wddm_radv.dll` 72E1D192 | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| x86, deployed `wow64\d3d11\amdgpu_wddm_radv.dll` E03A79CA | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| x64, BD-068 fix candidate 45712A13 | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| x86, BD-068 fix candidate A8A7FE3C | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| x64, b18 ICD 013DA4B0 (the BD-068 defect) | 16 PASS, 848 FAIL, 16 SKIP, exit 1 |

The b18 row is the negative control on real hardware. Only the small updates into host memory pass. Every
other case writes wrong bytes, and 348 cases also write before `dstOffset`. That run did not fault the GPU.

Ufaj, ale sprawdzaj. (Trust, but check: the driver reports success either way.)
