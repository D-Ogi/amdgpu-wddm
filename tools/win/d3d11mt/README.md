# d3d11mt

A multithreaded D3D11 client with an exact result. The owner asked (2026-09-29) that multithreading on the system
D3D11 path be tested with our own clients before any game is trusted to exercise it; this is that client. It renders
offscreen only, so it runs over SSH in session 0 as well as on the desktop, and it creates its device without
`D3D11_CREATE_DEVICE_SINGLETHREADED`: the runtime's thread-safe device is the subject.

## Build

```
pwsh tools\win\d3d11mt\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
```

Compiles with `/W4 /WX`, keeps the previous executable under `retained\` by its hash, checks `--help` and prints the
SHA-256 a lab runner pins. Output: `<BC250_ROOT>\scratch\build\d3d11mt\d3d11mt.exe`.

## What it does

The image is an `R32_UINT` render target of `threads x tiles` tiles (8 per row, `--tile` pixels square). Each tile
belongs to one worker thread.

1. **Deferred contexts** (`--rounds`, default 48). Per round, every worker, in parallel with the others, creates
   one fresh immutable 4x4 source texture per tile with initial data (concurrent creates), maps its own dynamic
   constant buffer with `WRITE_DISCARD` on its deferred context, draws each tile, and finishes a command list.
   The main thread executes the lists in worker order, flushes, and waits for the GPU every eighth round. Rounds
   ping-pong between two images: each pixel hashes its predecessor, so every round of every tile reaches the result.
   Worker references to the source textures are released at once, while the command lists still hold them.
2. **Concurrent creator** (on unless `--no-creator`). During phase 1 a further thread creates and releases
   textures and buffers with initial data, shader resource views and pixel shaders as fast as it can.
3. **Immediate context from every thread** (`--immediate-rounds`, default 16). The workers take turns on the
   immediate context under the application's lock, as D3D11 requires: `UpdateSubresource`, `CopySubresourceRegion`
   into a mosaic texture, `Map(WRITE_DISCARD)`, state and a draw per tile. The driver sees one context driven from
   several OS threads.

The shaders use integer arithmetic only, so every conforming implementation writes the same bits. The checksum
(FNV-1a 64 over phase 1's final image, phase 2's target and the mosaic) depends only on the total tile count, the
tile size and the round counts, never on how the tiles are split between threads: `--threads 1 --tiles 32` and
`--threads 4 --tiles 8` must agree, which makes a single-threaded run the oracle of a multithreaded one.

Reference checksums from the development PC (RTX 4090 native driver and WARP agree bit for bit; native reports
`driver_command_lists` true, WARP false, so both the driver's and the runtime's command lists are covered):

| Arguments | Checksum |
|---|---|
| defaults (4 x 8 tiles of 32, 48 rounds, 16 immediate) and `--threads 1 --tiles 32` | `db13de5647af4c51` |
| `--threads 8 --tiles 6` | `d4fae0ac0d58fad0` |
| `--threads 8 --tiles 8 --rounds 1000 --immediate-rounds 200` and `--threads 2 --tiles 32 ...` | `488a0a4bdef268c7` |

## Output and exit codes

A summary line (`D3D11MT ... checksum=...`, then `PASS` or `FAIL ...`) and one JSON object, format 1, on standard
output or in `--out FILE` (written atomically): settings, adapter (vendor, device, LUID, description), feature
level, `threading` (`driver_concurrent_creates`, `driver_command_lists`), image size, creator iterations, phase
times, `device_removed_reason`, the checksums, `d3d11` (`system` or `app-local`) and `modules`: path and SHA-256 of
every loaded D3D runtime, DXGI and Vulkan loader module, every `bc250*` and `amdgpu_wddm_*` module, anything from the
driver store, and every Vulkan driver. The module list is the route witness: on the BC-250 the system GPU path shows
the router and `amdgpu_wddm_d3d11.dll` with its engine and ICD, the CPU path `bc250d3d.dll`.

Exit codes: 0 pass, 1 API failure, 2 bad arguments, 3 deadline (`--deadline`, default 120 s, at most 170 s, enforced by
a watchdog that ends the process), 4 device removed, 5 checksum differs from `--expect`.

Kto pyta, nie błądzi. (Who asks does not go astray: the checksum asks every thread.)
