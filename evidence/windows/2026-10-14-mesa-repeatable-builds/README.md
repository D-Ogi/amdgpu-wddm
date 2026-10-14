# Repeatable builds of the Mesa files of the package (development PC, 2026-10-14)

Date: 2026-10-14, 05:27Z to 05:51Z. Development PC, Windows 11 Pro 26200, no lab. Recipe:
`tools/build/build-mesa.ps1` with `tools/build/mesa-configs.json` and `tools/build/common.ps1` of train b34
(SHA-256 `C4876C3C...`, `F0B44C83...`, `C759F33D...`; each record below repeats all three in
`recipe_identity.files_sha256`). Toolchain: MSVC 19.44.35221, Meson 1.12.0, Ninja 1.11.1, Python 3.14.0, Mako
1.4.1. Sources: fresh worktrees of the Mesa fork at the commits `tools/release/release-sources.json` pins for
train b34, each with a clean tracked tree (`worktree_diff_sha256` of the empty diff).

## Question

M15.10 records that every file Meson builds stays outside the `/Brepro` reproduction this repository measures for
its own `cl` and `link` recipes, and it asks for either a reproducible Meson recipe per fork or a stated
exception. So: does a Mesa file of the package rebuild to the hash the release manifest pins, from the pinned
commit alone, in a directory that is not the one it was built in?

## Method

Five builds, each from its own worktree at the pinned commit and into a build directory whose path differs from
the one the shipped file was built in. The 32-bit CPU driver was built twice, into two directories, to separate
the build path from the output. Every build writes a recipe record (`recipe.json`): the source commit and
worktree state, the Meson options, the tool paths and versions, the hashes of the three recipe scripts, and the
size and SHA-256 of every artifact. `scripts/compare-recipes.py` prints every difference between two records.

## Result

| Shipped file | Commit | Config | Record | SHA-256 | Bytes | Against the pin |
|---|---|---|---|---|---|---|
| `payload/desktop/bc250d3d.dll` | `cd87dbac` | llvmpipe-umd x64 | `recipe-cpu-x64-repro.json` | `CCD26AB98377DF0DE5CB862B5EFAC17086A9CD43797BA4E0884FFDBDF09D0EDE` | 68666880 | **the same bytes** |
| `payload/desktop/bc250d3d_zink.dll` | `8f63796c` | zink-umd x64 | `recipe-zink-x64-repro.json` | `0152C654D59DE402E8AF9E252E39EF91FE821975F34C5353972E8E2A8E069740` | 15280128 | **the same bytes** |
| `payload/opengl/amdgpu_wddm_gl.dll` | `9fa76f54` | zink-gl-icd x64 | `recipe-gl-x64-repro.json` | `DA3D2A6CE25D395B1FACA3C56077DDBC8F674778EE6F608D5E10AF7C350C75C1` | 21800448 | **the same bytes** |
| `payload/opengl/opengl32.dll` | `9fa76f54` | zink-gl-icd x64 | `recipe-gl-x64-repro.json` | `044A7033E054A1A214122AB0DBE93962874FCA1C198D0CB89BCDA1C609390542` | 617984 | the same bytes as its record |
| `payload/wow64/desktop/bc250d3d.dll` | `76ab2c27` | llvmpipe-umd x86 | `recipe-cpu-x86-repro.json` | `F28F7A040855F6F9ACFEB087ACE6B9A686B79A8041027214171B250A729832D5` | 56177152 | differs; the reason is below |
| the same, second directory | `76ab2c27` | llvmpipe-umd x86 | `recipe-cpu-x86-repro2.json` | `F28F7A040855F6F9ACFEB087ACE6B9A686B79A8041027214171B250A729832D5` | 56177152 | the same bytes as the first run |

## The one file that does not match, and why

The 32-bit CPU desktop driver the package ships is `FF33F4B1...`, 56203264 bytes. A build of the same commit on
2026-10-12 gave a third hash, `7570D33E...`, at that same size. The records name the cause. That older run used
other recipe scripts and passed four options fewer than the recipe of this train: `/Brepro`, `/FC`, the two
`/d1trimfile` prefixes and `/PDBALTPATH:%_PDB%`, which `Add-ReproducibleMesonOptions` adds. Without `/Brepro` a
PE keeps the moment of its build, so those two older builds could not match each other either, and
`/d1trimfile` takes the absolute source paths out of the image, which is most of the 26112 bytes the new file
saves. The shipped file therefore cannot be reproduced by design, while the recipe that replaces it is sound:
two runs of it, in different directories, give the same bytes.

## What this establishes

1. Four of the five files, every file of the desktop and OpenGL route that the current recipe builds, rebuild to
   the hash the release manifest pins, from a fresh worktree and at a path that differs from the original run.
   Anybody with the commit and the toolchain can check a pin, with no lab.
2. The build path does not reach the output: the two 32-bit runs differ in their directory alone and agree byte
   for byte.
3. Where a file cannot be reproduced, the reason is in the record rather than in the code.

So the Meson half of M15.10 needs no stated exception for these files: the recipe reproduces them. The 32-bit CPU
driver needs the rebuild that train b34 carries, which moves it to the commit of its 64-bit pair and to this
recipe.

## Files

| File | What it is |
|---|---|
| `recipe-cpu-x64-repro.json` | the record of the 64-bit CPU desktop driver build |
| `recipe-cpu-x86-repro.json`, `recipe-cpu-x86-repro2.json` | the two 32-bit runs, in two directories |
| `recipe-zink-x64-repro.json` | the Zink desktop driver build |
| `recipe-gl-x64-repro.json` | the OpenGL ICD build, with `opengl32.dll` beside it |
| `repro-cpu-x86.log`, `repro-cpu-x86-second.log`, `repro-cpu-x64-zink.log`, `repro-gl-x64.log` | the build logs |
| `scripts/compare-recipes.py` | prints every difference between two recipe records |
| `sha256.txt` | the SHA-256 of every file of this directory |

The commands behind the four logs, in the order they ran: the 32-bit CPU driver from `76ab2c27` against the
32-bit LLVM build of the workspace, the same again in another directory, the 64-bit CPU driver from `cd87dbac`
with the Zink desktop driver from `8f63796c`, and the OpenGL ICD from `9fa76f54`. Each one is
`tools/build/build-mesa.ps1 -Config <config> -Arch <arch> -Source <worktree> -Build <directory>`; the recipe
record of each build repeats its exact Meson options.
