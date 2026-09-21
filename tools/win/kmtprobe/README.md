# kmtprobe - ADR 0008 stage B from user mode

One user-mode tool that drives the full WDDM miniport through raw `D3DKMT*` calls (exported from
`gdi32.dll`, headers from the SDK NuGet package): find our adapter, create a device and a paging queue,
create one allocation, give it **the GPU virtual address we asked for**, make it resident, lock it, write a
pattern, unlock, print a machine-readable line, and tear everything down in reverse.

It is the "small host-side tool calling D3DKMT directly" of `docs/research/m7-full-wddm-miniport.md`
section 5.2. Nothing is submitted and no context is created: that is stage C (section 5.3). The steps are
one function each so a later `--submit` can be added between the lock and the hold without touching them.

The witness stays independent (ADR 0007 point 5): `bc250rd` walks the GPU page tables and reads the
physical pages, and the `RESULT` line is what tells it where to look.

## What each step exercises in the KMD

| Step | D3DKMT call | KMD DDI |
|---|---|---|
| 1 | `EnumAdapters2`, `QueryAdapterInfo(ADAPTERREGISTRYINFO, UMDRIVERNAME)`, `OpenAdapterFromLuid` | none |
| 2 | `QueryAdapterInfo(QUERY_GPUMMU_CAPS, NODEMETADATA 0, DRIVERVERSION, WDDM_2_0_CAPS)` | `DxgkDdiGetNodeMetadata`; the rest is dxgkrnl answering from our caps |
| 3 | `CreateDevice`, `CreatePagingQueue` | `DxgkDdiCreateDevice` |
| 4 | `CreateAllocation2` | `DxgkDdiCreateAllocation` |
| 5 | `ReserveGpuVirtualAddress` (with `--reserve`), `MapGpuVirtualAddress` | `DxgkDdiBuildPagingBuffer` (`UPDATE_PAGE_TABLE`, `FLUSH_TLB`) |
| 6 | `MakeResident` | `DxgkDdiBuildPagingBuffer` (`TRANSFER`, `FILL`) |
| 7 | `Lock2`, write, `Unlock2` | none in GpuMmu; needs the allocation marked `CpuVisible` |
| 9 | `FreeGpuVirtualAddress`, `DestroyAllocation2`, `DestroyPagingQueue`, `DestroyDevice`, `CloseAdapter` | the matching Destroy DDIs |

Exit criterion of stage B: the VA we asked for is the VA we got, and the pattern shows up at the physical
address the page tables point at.

## The private driver data

`driver/kmd/wddm.c`, `Bc250WddmCreateAllocation`, refuses any per-allocation blob that is not exactly
`BC250_WDDM_ALLOCATION_PRIVATE` with the right magic and a non-zero `Size`. `kmtprobe.c` repeats that
structure verbatim and `build.ps1` compares the magic against the driver source at build time, so a
divergence is a build failure rather than a puzzling `STATUS_INVALID_PARAMETER` in the lab.

The blob describes the buffer as one linear row (`Height = 1`, `Pitch = Size`, `Format = A8R8G8B8`), which
keeps `Pitch * Height == Size` the way the KMD's own `GetStandardAllocationDriverData` builds it.

## Build

```
pwsh tools\win\kmtprobe\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\kmtprobe
```

`/W4 /WX`, x64, `/MT`, no dependency on a WDK or SDK installation. Output: `kmtprobe.exe`, one file, no DLLs.

## Run, on the target

```
python tools\win\target.py push P:\BC-250\scratch\build\kmtprobe\kmtprobe.exe --to C:\BC250\tmp
python tools\win\target.py run "C:\BC250\tmp\kmtprobe.exe --hold 20"
```

`target.py run` is enough for a single command; for a run that has to be wrapped (gate, evidence capture,
guard-log dump around it) write a `.ps1` next to the experiment and use `target.py ps <script.ps1>`, because
inline PowerShell through Git Bash gets mangled.

The gate has to be open (`EnableFullWddm`) and the driver started, or `EnumAdapters2` will list a
display-only adapter that has no paging queue to give.

### Options

| Option | Meaning |
|---|---|
| `--match <text>` | substring of adapter string / chip type / UMD name, default `bc250` |
| `--luid H:L` | pick by LUID in hex instead, exactly as the listing prints it |
| `--size <bytes>` | default `0x10000`; `0x`, `K`, `M`, `G` accepted, rounded up to 4 KB |
| `--va <address>` | address to ask for, default `0x10000000`; `0` lets VidMm choose |
| `--vamin`, `--vamax` | range bounds, only consulted with `--va 0` |
| `--reserve` | reserve the range first (64 KB-aligned base) and map inside it |
| `--resident-first` | `MakeResident` before the map instead of after it |
| `--no-write` | lock but leave the contents alone |
| `--hold <s>` | keep everything alive so the witness can read |
| `--fence-timeout <ms>` | per-wait paging fence deadline, default 5000 |
| `--timeout <s>` | watchdog on the whole process, default 30, raised to cover `--hold` |

No wait is unbounded: every paging fence poll has a deadline, and a watchdog thread terminates the process
if the whole run overruns. Exit codes: 0 all steps passed, 1 a step failed, 2 bad arguments, 4 the watchdog
fired. Teardown runs on every path.

### The output to keep

```
RESULT va=0x0000000010000000 size=0x10000 pattern=0x4243323530420000 words=8192 luid=... alloc=0x...
```

The pattern is `"BC250B"` in the high six bytes of every 64-bit word, the word index in the low two. A page
of it is unmistakable in a physical-memory dump and says which word of the allocation a given byte is.

## Hazards

- The first `MapGpuVirtualAddress` is the first time VidMm asks our `BuildPagingBuffer` for real work. Stage A
  answers every operation inertly, so a map can "succeed" with no PTE behind it; the witness, not the
  `NTSTATUS`, is what settles that. See `scratch/tmp/kmtprobe_kmd_patch.md` for the gap list.
- A GPU VA with no page table behind it is a fault waiting for stage C, not for this tool: nothing here lets
  the GPU touch the range.
- `--hold` keeps an allocation resident in the VRAM carve-out. Harmless at 64 KB, worth a thought at 1 GB.
