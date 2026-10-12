# Reproducible builds for the kernel driver and the D3D12 shell

M15.10 asks that every accepted artifact be built from a committed source revision in the public
repositories. Naming the revision turned out to be the easy half. The hard half is that rebuilding from that
revision does not give the same file, so "this deployed driver is that commit" cannot be checked by comparing
hashes. This note says exactly why, what to change, and what the signing step can and cannot be asked to do.

The first measurements below used WDK/SDK 10.0.26100 on one host, 2026-10-03. They do not prove that every later revision reproduces across build environments.

## What was measured, before the change

Two builds of one unchanged tree, nothing touched in between:

| artifact | size | differing bytes | where |
|---|---|---|---|
| `bc250kmd.sys` | 471952 | 361 (0.0765 %) | 320 in the certificate table, 30 in the CodeView record, 7 in the debug directory, 2 in the COFF `TimeDateStamp`, 2 in the optional header `CheckSum` |
| `amdgpu_wddm_d3d12.dll` | 921088 | 4 (0.0004 %) | 2 in the COFF `TimeDateStamp` and the same 2 echoed in the debug directory |

Every byte of code and data is identical in both. Four kernel-driver builds of the same sources gave
`693E9725`, `7AFDD538`, `8AC6ECFC` and `03DB82B5`, and three shell builds gave `8A56C1F7`, `F2751444` and
`5CA491EE`, all at the sizes above. So the non-determinism is small, it is entirely in metadata, and it is
fully enumerated.

The 30 bytes in the CodeView record are worth naming separately, because they are not a clock: 16 of them are
the PDB GUID, and 14 are the **absolute path of the PDB**. The two builds differed there only because they were
told to write their PDB to different output directories. The same sources built to a different `-Out` give a
different `.sys` today. The deployed driver `22FDE5B8` carries
`...\scratch\m15\kmd-held\build196\kmd\bc250kmd.pdb` in that record, which is one builder's directory layout
baked into a shipped binary.

## The causes, and what each one needs

| # | cause | bytes | fix |
|---|---|---|---|
| 1 | COFF `TimeDateStamp`: the linker writes the clock | 2-4 | `/Brepro` on `cl.exe`, `link.exe` and `lib.exe`. The linker then writes a hash of the output instead of a time, and adds an `IMAGE_DEBUG_TYPE_REPRO` entry saying so |
| 2 | the same timestamp echoed in the debug directory | 7 | the same flag; the debug directory follows the header |
| 3 | the PDB GUID and the PDB path in the CodeView record | 30 | `/Brepro` derives the GUID from the content; `/PDBALTPATH:%_PDB%` embeds the file name only, not the directory |
| 4 | the optional header `CheckSum` | 2 | follows from 1-3: identical content gives an identical checksum |
| 5 | the Authenticode signature in the certificate table | 320 | follows from 1-4 as well, for this signing setup. See below |
| 6 | the object list given to the linker is an unordered directory enumeration | 0 here | `/OPT:ICF` folds identical functions in input order, so a different order is a different image. Sort the list; it costs nothing and removes the risk |

Cause 6 did not show up in these two builds because the enumeration order happened to be stable. It is in the
list because nothing in `Get-ChildItem` promises that.

## What was measured, after the change

The two builds measured on that host were identical, including the signature:

| artifact | build 1 | build 2 |
|---|---|---|
| `bc250kmd.unsigned.sys` (471040 bytes) | `D6CAE34C` | `D6CAE34C` |
| `bc250kmd.sys`, signed (472464 bytes) | `8022ED6B` | `8022ED6B` |
| Authenticode PE hash of the signed driver | `4F5E9C1AA87AAB20...` | `4F5E9C1AA87AAB20...` |
| `amdgpu_wddm_d3d12.dll` (921088 bytes) | `D590BB41` | `D590BB41` |

The signature reproducing was not expected and is worth stating precisely, because it holds only under
conditions that are easy to lose. `signtool sign` is invoked here without `/t` and without `/tr`, and in that
mode it writes no `signingTime` attribute at all: the only two `UTCTime` values in the PKCS#7 blob are our test
certificate's own validity dates. RSA PKCS#1 v1.5 signing is deterministic. So one image plus one key gives one
signature. Add a timestamp countersignature, or sign with a different certificate, and the signed file stops
reproducing while the image underneath still does.

The catalog does not reproduce, and cannot: 306 of its 2407 bytes differ between the two builds, and they are a
random 16-byte catalog identifier under OID 1.3.6.1.4.1.311.12.1.1, the `Inf2Cat` time stamp, and the signature
over both. `/uselocaltime` is dropped so that the time it does carry is UTC rather than the builder's time zone,
but that is hygiene, not determinism.

## What changes in the build scripts

`driver/kmd/build.ps1`:

- add `/Brepro` to `$clFlags`;
- add `/Brepro` and `/PDBALTPATH:%_PDB%` to the `link.exe` arguments;
- sort the object list: `(Get-ChildItem "$obj\*.obj" | Sort-Object -Property Name).FullName`;
- copy the linked image to `bc250kmd.unsigned.sys` **before** `Inf2Cat` and `signtool`, so that the
  signing-independent artifact exists as a file and can be hashed;
- drop `/uselocaltime` from both `Inf2Cat` calls.

`tools/build/build-umd-d3d12.ps1`: add `/Brepro` to `$flags` and `/Brepro` after `/link`.

`tools/build/build-engine-ddi.ps1`: add `/Brepro` to `$flags` and to both `lib.exe` calls; `engine-ddi.lib` is
linked into the shell, so an archive with member timestamps would defeat the shell's determinism.

`cl.exe`, `link.exe` and `lib.exe` of WDK/SDK 10.0.26100 all accept `/Brepro`; the header gates and the host
tests pass with it.

## What the signing step needs

1. **Gate on the unsigned image.** `build.ps1` already writes `source-manifest.json` with a SHA-256 per source
   file. Add the SHA-256 of `bc250kmd.unsigned.sys` and of the shell DLL. A rebuild from a named revision then
   either matches that hash or does not, with no signing in the way. This is the gate that keeps working if a
   timestamp countersignature is ever added.
2. **Compare a deployed, signed driver by its Authenticode PE hash, not by its file hash.** What is deployed is
   a signed file, and the Authenticode hash deliberately skips the certificate table, the `CheckSum` field and
   the security data-directory entry, so it is identical for two signings of one image. A small tool parses the
   PE and hashes the file with those three regions excluded; `tools/win/stackbudget.py` already walks these
   headers, so the parsing is not new ground. This was validated against `signtool`'s own output on three
   signed drivers, the deployed `0.7.196.1` among them: the digest computed this way is the digest `signtool`
   embedded. It only helps together with `/Brepro`, because the timestamp sits inside the hashed region.
3. **Record the catalog; do not gate on it.** Keep its hash in the build record for identification, with a line
   saying it is expected to differ per build and why.
4. **Do not add `/t` or `/tr` to `signtool` without updating this note.** A countersignature would move the
   signed driver from "reproduces" to "reproduces under the Authenticode hash only". That is an acceptable
   trade if a trusted signing time is ever wanted; it is not an accident to stumble into.
5. A byte-identical signature under a *different* key or with a pinned time would need a tool that takes the
   signing time and the private key as inputs (`osslsigncode -time`). That is a change of trust model for a
   test certificate and it buys nothing over steps 1 and 2. Not proposed.

## The implemented gates

`build.ps1` saves `bc250kmd.unsigned.sys` before catalog creation and signing. Its package path captures
`source-manifest.start.json`, verifies that source identity did not change, and writes `source-manifest.json`
with package hashes and the hash of `compile_commands.json`. It does not write `reproducible.json`, and there
is no `tools/quality/reproducible.py` gate.

`tools/quality/test_kmd_reproducible.py` checks the production ordinal ordering helper under PowerShell 5.1
and 7, with three cultures. Its negative control restores `Sort-Object Name` and must fail a behavioral check.
The named `kmd-order` gate in `quick.ps1` runs this check before other quality gates. The `quality-controls`
gate also discovers `test_kmd_package.py`, which runs the positive and negative controls. These gates check
input ordering. They do not compile two drivers or certify their binary equality.

The 2026-10-10 review of BD-114 found that the two PowerShell versions ordered nine object name families
differently, at 22 positions. These include `bc250_sdma` and `dcn`. The effect is not specific to SDMA.
Relinking the same objects in those two orders reproduced the two reported unsigned hashes, `69111A1C` and
`D1D2F1A9`. The recipe now orders source and object paths with `StringComparer.Ordinal`. A promotion must still
compare complete unsigned rebuilds with recorded compiler, linker, kits and environment inputs. Ambient
compiler options, different tool binaries and untested environments are outside the ordering gate.

## Package builds from reviewed worktrees

The package path resolves its workspace from explicit `-QualityWorkspace`, then `BC250_ROOT`, then the
local repository configuration `bc250.workspace`, then a `-Kits` path of the form `<workspace>/toolchain/nuget`.
A relative repository setting is relative to the repository root. An invalid selected setting fails. It does
not fall back to a different workspace. Because some host tests accept only a workspace root, both quality
profiles refuse kits outside that workspace's `toolchain/nuget`. `package-context.json` records the package
choice, and the quality result records the context its gates used.

The default quality profile remains `Full`. It requires the tracked status maps to match the current source
and WDK headers. A package uses `KmdPackage`: the same generator checks current source and headers, but
writes its maps below the quality output directory. Its receipt records differences from tracked maps and
marks only tracked presentation freshness as a profile exclusion. It does not edit the reviewed commit.
The status-map tests, source identity checks, compiler and contract gates, and other quality checks still run.
A generator error remains a package failure.

## One thing to be careful about

Turning these flags on changes the bytes of the driver. A `/Brepro` build of the kernel driver is a new
artifact: it must go through the ordinary promotion path on the lab, with its own candidate, postflight and
rollback, and not be swapped in quietly because "only metadata changed". The flags touch the debug directory
and the PE header of a file whose signature the boot loader checks; the cost of being wrong about that is a
machine that does not start. The shell is cheaper to try and should go first.

## What this does not solve

- A different WDK/SDK, a different MSVC toolset or a different `/kitVersion` gives a different image, by
  design. `reproducible.json` records them so a mismatch is identified rather than mysterious.
- Artifacts already deployed, including the kernel driver `22FDE5B8` and the shell `5A1B7BAF`, were built
  without these flags. They cannot be reproduced bit for bit, and that stays true. What can be done for them is
  what the integration branch already does: prove the per-file source manifest is identical to the deployed
  build's, which fixes the sources exactly even though it does not fix the bytes.
- The Meson builds of Mesa, DXVK and vkd3d-proton are in the next section.

## The release payload, 2026-10-08

The sections above cover the kernel driver and the 64-bit D3D12 shell. The release package has 33 payload files
from 13 recipes. This section gives the result for each recipe that builds a payload file. It also lists the
shipped files that do not reproduce, with the reason for each. The machine-checked record of each file is in
`tools/release/release-sources.json`, and `tools/release/README.md` ("Provenance") describes its fields and gate.

### Two more causes

The images of the KMD and the D3D12 shell hold no path of the build tree. The images of other recipes hold one in
two forms, and `/Brepro` removes neither of them.

| # | cause | where it was measured | fix |
|---|---|---|---|
| 7 | `__FILE__` holds the absolute path of the source file, and logging and `assert` put it into the image | the two D3D11 shells: 25 strings with the path of the build tree | `/FC` makes every `__FILE__` absolute. `/d1trimfile:<dir>` then removes that directory from the start of the path |
| 8 | the name of an anonymous namespace holds a hash of the absolute path of its source file (`?A0x` and eight hex digits) | `d3d11bench.exe`: three differing ranges. `mfthost.exe`: other function order, because the linker orders COMDATs by these names | the same `/d1trimfile` switch. The compiler then makes the hash from the trimmed path |

`/d1trimfile` is not documented. It works with MSVC 14.44.35207 (`cl.exe` 19.44.35221), and it accepts more
than one directory. `/pathmap` changes only the paths in the debug information, not `__FILE__`. A recipe gives one
`/d1trimfile` for each root directory of its sources and generated files.

### The MSVC recipes of this repository

Each recipe built two times, from two work trees of one commit, into two output directories with paths of
different lengths. "Before" is main `9c03c35c`. "After" is the branch at `51b1e876` (`10a4d091` before the
rebase of this branch onto main `5fc83d67`).
A hash is the first 8 hex digits of the SHA-256.

| recipe | payload file | added switches | before: build 1, build 2 | after: both builds |
|---|---|---|---|---|
| `tools/build/build-umd-router.ps1 -Arch x64` | `bc250d3d_router.dll` | `/Brepro`, `/FC`, `/d1trimfile`, `/link /Brepro` | `B7227BDB`, `9554303A` | `F1E43918` |
| `tools/build/build-umd-router.ps1 -Arch x86` | `bc250d3d_router.dll` (x86) | the same | `0D902587`, `0D80F110` | `E93AFEFA` |
| `tools/win/bc250kmd_cli/build.ps1` | `bc250kmd_cli.exe`, `bc250control.dll` | the same | `CC50685E`, `5932B785` and `A0362B03`, `56F08918` | `E7C248E5` and `570A77F4` |
| `tools/win/amdgpu_wddm_control/build.ps1` | `amdgpu_wddm_control.exe`, `bc250control.dll`, `bc250kmd_cli.exe` | a sorted list of C# sources and resources | `74833F09`, `570A77F4`, `E7C248E5` in both | the same |
| `tools/win/amdgpu_wddm_setup/build.ps1` | `amdgpu_wddm_setup.exe` | a sorted list of C# sources and resources | `09F6A040` in both | `09F6A040` |
| `tools/win/d3d12caps/build.ps1` | `amdgpu_wddm_d3d12caps.exe` | `/Brepro`, `/FC`, `/d1trimfile`, `/link /Brepro` | `56A01BE6`, `D437AF35` | `4C9032BC` |
| `tools/win/d3d11bench/build.ps1` | `d3d11bench.exe` (not in the package now) | the same | `188CA4C7`, `C6E33241` | `D27DA5DE` |
| `driver/umd/mft-h264/build.ps1` | `amdgpu_wddm_mft_h264.dll` | `/FC`, `/d1trimfile` | `D3E29E6E` in both (`mfthost.exe`: `5B47BBDF`, `E316E891`) | `D3E29E6E` (`mfthost.exe`: `5B47BBDF`) |
| `tools/build/build-umd-dxvk.ps1 -Arch x64` | `amdgpu_wddm_d3d11.dll` | `/FC`, `/d1trimfile` | `90F3C6D2`, `DB3A920F` | `6EF1E349` |
| `tools/build/build-umd-dxvk.ps1 -Arch x86` | `amdgpu_wddm_d3d11.dll` (x86) | the same | `BF7E139E`, `710F2E21` | `DECDFE58` |
| `tools/build/build-umd-d3d12.ps1 -Arch x64` | `amdgpu_wddm_d3d12.dll` | `/FC`, `/d1trimfile` | `A3F8E2A8` in both | `A3F8E2A8` |
| `tools/build/build-umd-d3d12.ps1 -Arch x86` | `amdgpu_wddm_d3d12.dll` (x86) | the same | `0A9BD879` in both | `0A9BD879` |
| `driver/kmd/build.ps1` | `bc250kmd.sys` | none | unsigned `32F97DDC`, signed `CFBD8A64` in both | the same |

The control application and the setup window are C# programs. `csc /deterministic+` already gives the same bytes
in another directory, and the sorted lists only remove a dependence on the order of a directory listing. The two
D3D11 shells are 11 KB smaller after the change, because each `__FILE__` string is shorter.

Two outputs that are not payload files still differ between two builds of the D3D12 shell recipe: `engine-ddi.lib`
and the host test programs that link it. `lib.exe` writes the absolute path of each object file into the archive,
and the objects carry their own path in the debug information. The shell DLL does not change, because the linker
copies neither of them into the image.

### The Meson recipes of the forks

The recipes `tools/build/build-mesa.ps1`, `build-dxvk.ps1` and `build-vkd3d.ps1` configure Meson. From `308a689d` on
(`841e4018` before the rebase), each recipe calls `Add-ReproducibleMesonOptions` in `tools/build/common.ps1`. This function adds
`/Brepro /FC /d1trimfile:<source> /d1trimfile:<build>` to `c_args` and `cpp_args`. It adds
`/Brepro /PDBALTPATH:%_PDB%` to `c_link_args` and `cpp_link_args`. Meson keeps only the last `-D` value of an
option, so the function adds the switches to the value that the recipe gives and does not give a second value.
The configure gate of each recipe then compares the "Build Options" line of Meson with the merged set. The source
and build directories must not contain white space, because Meson divides `c_args` at white space.

`rebuild-check.ps1 -Twice` built one payload entry of each fork two times with 8 parallel jobs. The two builds used
two copies of one commit and two output directories with paths of different lengths. "Before" is the recipe of
the commit that the manifest of main names (`3c31bd97`, `d5593267`, `693c02b3`). "After" is the recipe of
`308a689d`.

| fork commit | recipe | payload file | before: build 1, build 2 | what differs before | after: both builds |
|---|---|---|---|---|---|
| vkd3d-proton `4e9a98e9` | `build-vkd3d.ps1 -Config ddi-engine-lto -Arch x64` | `d3d12/amdgpu_wddm_vkd3d.dll` | `4C304B6B`, `ADC5AD40` | 4 bytes: the COFF time stamp, the debug directory time stamp and the CheckSum | `59AB2573` |
| Mesa `31844893` | `build-mesa.ps1 -Config radv-mt` | `d3d12/amdgpu_wddm_radv.dll` | `FCE3AB59`, `59D6C87E` | 102 bytes: the time stamps, the CheckSum, the RSDS GUID, the PDB path and one absolute source path of 39 bytes | `88A8F594` |
| DXVK `5611118e` | `build-dxvk.ps1 -Config ddi-engine` | `d3d11/amdgpu_wddm_dxvk.dll` | `D7A65A98`, `A45F1F2C` | 434222 bytes in 9664 ranges, most of them in `.text` and `.rdata`: the functions are in another order | `E506862E` |

The DXVK image holds the name of one anonymous namespace, and its path hash (cause 8) is different in the two
builds before the change. After the change the two builds are equal.

The other fork entries of the manifest, built two times in the same way with the recipe of `308a689d`:

| fork commit | recipe | payload file | after: both builds |
|---|---|---|---|
| Mesa `7eb7861d` | `build-mesa.ps1 -Config zink-umd -Arch x64` | `desktop/bc250d3d_zink.dll` | `6C3134EB` |
| Mesa `48546c73` | `build-mesa.ps1 -Config radv` | `desktop/amdgpu_wddm_radv.dll` | `D0CBD7F2` |
| Mesa `76ab2c27` | `build-mesa.ps1 -Config llvmpipe-umd` | `desktop/bc250d3d.dll` | `DF43D302` |
| Mesa `76ab2c27` | `build-mesa.ps1 -Config llvmpipe-umd -Arch x86` | `wow64/desktop/bc250d3d.dll` | `F28F7A04` |
| Mesa `31844893` | `build-mesa.ps1 -Config radv-mt -Arch x86` | the three x86 RADV files in `wow64/` | `0863C0AC` |
| DXVK `5611118e` | `build-dxvk.ps1 -Config ddi-engine -Arch x86` | `wow64/d3d11/amdgpu_wddm_dxvk.dll` | `7F48A72E` |
| vkd3d-proton `bdec00f3` | `build-vkd3d.ps1 -Config ddi-engine-lto -Arch x86` | `wow64/d3d12/amdgpu_wddm_vkd3d.dll` | `3DE7A5C7` |
| Mesa `2732f9c8` | `build-mesa.ps1 -Config radv` | `vulkan/vulkan_radeon.dll` | the build stops |

The two `llvmpipe-umd` builds read the same local LLVM build. This measurement does not show that an LLVM build in
another directory gives the same bytes.

The `radv` option set has `-Db_ndebug=true` from `b4c74c86` on. With `NDEBUG`, line 714 of
`src/compiler/spirv/vtn_cmat.c` at `2732f9c8` declares a variable that only `assert` reads. Mesa makes warning
C4189 an error (`/we4189`), so the build stops. The shipped file was built with the option set of `d6176765`,
which does not set `b_ndebug`. The system ICD stayed on its Mesa line, so `build-mesa.ps1 -Config radv-system`
(`1eb9ddc3`, on main as `a2ffa26c`) named that option set: the `radv` set without `-Db_ndebug=true`. From Mesa
`a7f44c96`, which relaxes C4189 in MSVC `NDEBUG` builds, the `radv-system` set has `-Db_ndebug=true` as well,
and the two measurements below describe the b23 file, not a file built with the current set.

The b23 system ICD `A6562DAF` is Mesa `308a5e33` (`2732f9c8` and one BD-096 fix), built with the recipe of
`d6176765`. Two measurements:

- Two builds of `308a5e33` with `-Config radv-system` give `24EC5444`. This image is 35840 bytes shorter than
  `A6562DAF`, because `/FC` and `/d1trimfile` make the `__FILE__` strings shorter. The `.text` section has the
  same size, 10814464 bytes.
- One build of `308a5e33` with the recipe of `d6176765`, in directories whose paths have the same lengths as the
  b23 directories, has the size of `A6562DAF`. The differences are the per-build fields and the directory name in
  85 path strings: 8 in 8-bit characters, including the PDB path, and 77 in UTF-16, from `assert`. With the
  b23 directory name in those strings and the per-build fields of `A6562DAF`, the result is `A6562DAF`. The
  CheckSum of the result is equal to the CheckSum of `A6562DAF`.

A build with the new recipes is a new artifact. It does not give the bytes of a shipped fork file, because the
shipped files were built without `/Brepro`:

- The vkd3d-proton engine `348117F1` is equal to its rebuild with the recipe of `3c31bd97`, apart from the time
  stamps and the CheckSum. `pe_compare.py` accepts this, but `/Brepro` changes 169981 bytes of the image.
- The RADV ICD `822134D0` is 3072 bytes longer than its rebuild with the recipe of `d5593267`. Its 472 `__FILE__`
  strings start with `../wagon-icd/src`, the name of the source tree of the original build, where the rebuild
  has `../src/src`. The `.text` section has the same size, 9874208 bytes.
- The DXVK engine `C242BA6A` differs from its rebuild in 855204 bytes. The function order depends on the path of
  the original build tree, so a rebuild in another directory cannot give these bytes.

What remains for the forks:

1. The next release train builds each fork file with the recipe of `308a689d` or later, from its published commit.
   Then `rebuild-check.ps1` can tie the file, and the manifest entry can drop its `unverified` field.
2. Each fork file must have a `recipe.json` that names a committed recipe. Some shipped files came from an
   uncommitted copy of the recipe or from a build script outside the repository.
3. The `llvmpipe-umd` builds link a local LLVM build (`scratch/llvm2312-build` and its x86 copy). A rebuild needs an
   LLVM build that a commit or a download record identifies.
4. `vulkan/vulkan_radeon.dll` builds with `-Config radv-system`. Up to b23 that set had no `-Db_ndebug=true`,
   because its Mesa line did not build with `NDEBUG`. From Mesa `a7f44c96` the set has it, and the system ICD
   of `0.7.216.100-tester.23` is the first one built with it. That file reproduces, and the section below
   measures it.

### The shipped files of main 9c03c35c

This section is the first measurement of the whole payload, made on the release that main `9c03c35c` named.
The payload of `0.7.216.100-tester.23`, which the manifest in this tree describes, is the section after it.

`tools/release/rebuild-check.ps1` built each payload entry of main `9c03c35c` again from the commit that its
`built_from` record names, in new work trees under `scratch\repro-builds`. Ten files gave the shipped bytes:

| payload file | commit | SHA-256 |
|---|---|---|
| `kmd/bc250kmd.sys`, signed | `a00da18c` | `2A858552` |
| `d3d12/amdgpu_wddm_d3d12.dll` | `bc1b00a9` | `477373C1` |
| `wow64/d3d12/amdgpu_wddm_d3d12.dll` | `41919147` | `077104FE` |
| `d3d11/amdgpu_wddm_d3d11.config` | `bc1b00a9` | `0494BA88` |
| `wow64/d3d11/amdgpu_wddm_d3d11.config` | `bc1b00a9` | `858FDE4F` |
| `tools/bc250kmd_cli.exe` | `a00da18c` | `E7C248E5` |
| `control/amdgpu_wddm_control.exe` | `a00da18c` | `74833F09` |
| `control/bc250control.dll`, `tools/bc250control.dll` | `a00da18c` | `570A77F4` |
| `mft/amdgpu_wddm_mft_h264.dll` | `e79f72e0` | `D3E29E6E` |

The other 19 built files have an `unverified` field in the manifest that gives the reason. The reasons are of three
kinds:

1. Built without `/Brepro`, from a commit that the record names. The rebuild is equal apart from the fields that a
   link without `/Brepro` writes for each build. `tools/build/pe_compare.py` shows this for the two routers
   (`9bc5818f`) and for `amdgpu_wddm_d3d12caps.exe` (`0d82a059`).
2. Built in a directory whose path is in the image. The two D3D11 shells hold the path of their build tree in 25 and
   26 `__FILE__` strings, so a rebuild in another directory has another length.
3. The fork builds (Mesa, DXVK, vkd3d-proton). None of them had `/Brepro`. Most images hold the paths of their
   build tree, and most of them came from a copy of the recipe that is not in a commit. The `recipe.json` next to the
   shipped file gives the hashes of that copy, where it exists.

A file of kinds 1 and 2 reproduces when the next release builds it again with the recipes of this branch. A fork
file reproduces when it is built with the recipes of this branch and its commit is on a published branch.

### The shipped files of 0.7.216.100-tester.23

`tools/release/release-sources.json` in this tree describes the payload of `0.7.216.100-tester.23`, which main
`5fc83d67` released. Of its 34 files, 30 are built by a recipe, 3 are data files of a commit, and one is the
LunarG `vulkaninfo.exe`. Nine of the 30 claim a bit-identical rebuild, and the other 21 say why they
cannot, with the recipe hashes or the switch that was missing.

Two of the built files are the ones this release itself built, and both reproduce. Each was built two times
from one commit of this branch, into two directories whose paths have different lengths.

| payload file | commit | recipe | registered | both builds |
|---|---|---|---|---|
| `kmd/bc250kmd.sys`, unsigned | `00f09bfa` | `driver/kmd/build.ps1` | `C8D69F59`, 668160 bytes | `C8D69F59` |
| `kmd/bc250kmd.sys`, signed | `00f09bfa` | the same | `2D0F5CB6`, 669584 bytes | `2D0F5CB6` |
| `kmd/bc250kmd.inf` | `00f09bfa` | the same | `444A5156`, 20866 bytes | `444A5156` |
| `vulkan/vulkan_radeon.dll` | mesa `d3da6d0a` | `build-mesa.ps1 -Config radv-system` | `4E3F393F`, 21914112 bytes | `4E3F393F` |

The column `registered` holds the hash of the source that the manifest registers for the release. The
release signs the kernel driver again and edits its INF, so the package holds other bytes for those two
files. The manifest keeps the Authenticode digest of the signed copy and names the edit of the INF, and
`provenance.py check --package` compares the package against both.

The kernel driver gives one more result than the table says. The shipped file came from a third output
directory and from `00f09bfa`, while the first build of the train came from `c0c7a0a1`, the commit before it,
which changes an identifier inside a comment of a compiled header. Three directories and two commits of the
same sources therefore give one image. The catalog `bc250kmd.cat` is different in every build, because every
build signs a new one, and `/Brepro` cannot reach a detached signature.

The system Vulkan ICD is the first fork file of a release that reproduces. It needed two things that arrived
together: the `radv-system` option set with `-Db_ndebug=true`, and a Mesa line that builds with `NDEBUG`. The
shipped file was built with the recipe tree `998f4130`, whose `radv-system` set is equal to the set of this
branch, field for field and in order, and the reproducible switches reach it through
`Add-ReproducibleMesonOptions`.

Two more files of the payload reproduce without a build: the two D3D11 capability records. They are 132 bytes
each, and `tools/build/write-umd-config.py` writes them from the capability file of the probe and the hashes
of the engine and the ICD of their architecture. Run again on the b24 round-3 capability files, the recipe
gives `1E975948` (x64) and `A279A6CA` (x86), the bytes in the package.

The twenty-one other built files were made before this branch. Their reasons are of the same three kinds as
in the section above, with one addition: a build whose source tree was not clean. The `recipe.json` of the two
x64 engine builds of b23 counts five modified entries in the DXVK tree and three in the vkd3d-proton tree.
Each of those two worktree diffs is byte-equal to the diff from the recorded head to the next commit of the
same fork branch, so each build read the tree of a published commit, and the record names that commit. Each
file becomes verifiable the first time a train builds it with the recipes of this branch, from a clean tree of
that commit.
