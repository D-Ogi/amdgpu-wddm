# Reproducible builds for the kernel driver and the D3D12 shell

M15.10 asks that every accepted artifact be built from a committed source revision in the public
repositories. Naming the revision turned out to be the easy half. The hard half is that rebuilding from that
revision does not give the same file, so "this deployed driver is that commit" cannot be checked by comparing
hashes. This note says exactly why, what to change, and what the signing step can and cannot be asked to do.

Everything below is measured on this tree with WDK/SDK 10.0.26100, 2026-10-03.

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

Both artifacts reproduce bit for bit, including the signature:

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

## The gate

- `build.ps1` writes `reproducible.json` next to `source-manifest.json`: the unsigned image hash, the
  Authenticode PE hash of the signed image, the catalog hash, the toolset and kit versions, and the flags that
  were used.
- A new `tools/quality/reproducible.py`, wired into `quick.ps1`, rebuilds nothing. It checks that the flags are
  present, that `bc250kmd.unsigned.sys` exists, and, when given a pinned expectation, that the unsigned hash
  matches it.
- The promotion path gains one step: before a candidate is registered on the lab, rebuild it in a second
  directory and compare the unsigned hashes. Two directories is the case that caught the embedded PDB path.

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
- The Mesa ICD and the vkd3d-proton engine are built by Meson and are not covered here.
