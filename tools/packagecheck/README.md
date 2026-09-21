# packagecheck

Reads a finished `bc250kmd` driver package and says whether it is the package we think it is. Run it on
the package directory **before every install on the lab**, and keep its Markdown output with the run's
evidence.

E16 runs 001 and 002 were spent measuring a package whose INF had no `UserModeDriverName`. A full
(non compute-only) WDDM adapter cannot start without one on unit A's build of dxgkrnl (`docs/facts.md`
M64), so that package could never have passed, and two boots of the lab machine measured nothing.
`--model full` turns that into a one-second error here. *Mądry Polak po szkodzie* - the Pole is wise
after the damage; the point of this tool is to be wise before the next one.

## Usage

```
python tools/packagecheck/packagecheck.py <package-dir>
        [--model full|display-only]      what the package is for; inferred from a DLL when omitted
        [--expect-version 0.7.4]         the version this package should be
        [--expect-commit <hash>]         needs a BUILDINFO.json (see below)
        [--load]                         load the UMD stub here and call its entry points (see below)
        [--kits DIR]                     unpacked WDK/SDK NuGet root; default <workspace>\toolchain\nuget
        [--tmp DIR]                      scratch for the Inf2Cat copy; default <workspace>\scratch\tmp
        [--no-tools]                     skip signtool, infverif and Inf2Cat
        [--json out.json] [--markdown out.md] [--manifest out.txt] [-v]
```

`-v` also prints the checks that passed.

Typical, before an install:

```
python tools\packagecheck\packagecheck.py P:\BC-250\scratch\build\bc250kmd-074\package-umd ^
       --model full --expect-version 0.7.4 --load ^
       --markdown ...\evidence\windows\2026-09-21-E16-run-004\packagecheck.md ^
       --manifest ...\evidence\windows\2026-09-21-E16-run-004\package-sha256.txt
```

## This is the pre-install gate

`packagecheck` is the gate between a build and the lab machine. Nothing gets pushed to unit A before it
has run over the exact directory that is about to be pushed, with the `--model` of the run and the
`--expect-version` the run is supposed to measure. It needs no lab access, no BC-250 and no device: it
reads files.

**Exit codes**

| code | meaning | what to do |
|---|---|---|
| `0` | no error. Warnings and notes may still be printed. | Read the warnings, then install. |
| `1` | at least one **error**. The package is wrong in a way that has cost us lab runs before. | Do not install. Fix the build, run again. |
| `2` | the tool could not do its job: the directory does not exist, a file could not be read. | Not a verdict on the package. Fix the invocation. |

An exit code of 1 is not advisory. E16 runs 001 and 002 are what an ignored `UMD001` looks like: two
boots, two installs, two measurements of nothing.

Tests:

```
python -m unittest discover -s tools/packagecheck
```

The suite builds its own synthetic INFs and PE images, runs with the kit tools switched off, and adds a
few checks against the real packages under `scratch\build\bc250kmd-074` when they are there.

## Findings

Every finding carries a code (`UMD001`, `VRS011`, ...) so that a report can be cited in `docs/facts.md`.

- **error** - do not install this package.
- **warn** - probably wrong, decide before installing.
- **note** - a fact worth having in the evidence, including the ones that are correct on purpose
  (the missing `UserModeDriverNameWow`, the untrusted test certificate, the catalog-signed-only DLL).

## What is checked

**INF.** Parsed here, by hand: sections, line continuations, comments (a `;` inside quotes is not one),
`%Strings%` substitution, and UTF-8/UTF-16 BOM handling. `Signature`, `Class` and `ClassGuid` against the
Windows display class, `CatalogFile` present in the directory, `DriverVer` shape, `PnpLockdown`. The
hardware id `PCI\VEN_1002&DEV_13FE` must be in the models section, and any *other* hardware id the INF
matches is a warning - this driver is meant to match exactly one device.

**Service.** `<install>.Services` -> `AddService` -> the service-install section -> `ServiceBinary`,
its dirid, and the fact that the binary it names is also copied by a `CopyFiles` section. A service
pointing at a file the install never copies is an error.

**Files.** Everything in `CopyFiles` must be in `[SourceDisksFiles]` and in the directory; everything in
`[SourceDisksFiles]` must be in the directory. Every file in the directory must be referenced by the INF
or be a known extra (`.cat`, `.cer`, `.json`) - an unreferenced binary is a leftover from an older build
or a file somebody forgot to add to `CopyFiles`. SHA-256 of every file goes in the report.

**User-mode driver** (`--model full`). `UserModeDriverName` must exist in the *software* key (an AddReg
of the DDInstall section; one written from a `.HW` section lands in the device key, where dxgkrnl does
not look), be `REG_MULTI_SZ` (`0x00010000`), and name at least one DLL. Every DLL it names must be in
`CopyFiles`, in `[SourceDisksFiles]` and in the directory. `REG_SZ` (flags `0`) is a warning, not an
error: `bc250kmd.inf` keeps it as a documented fallback to try if the DLL is never loaded. A missing
`UserModeDriverNameWow` is a note - our INF omits it on purpose, because a Wow value pointing at the
64-bit stub would make "the runtime never asked for a 32-bit driver" and "it asked and the load failed"
the same observation. For `--model display-only` a missing UMD name is a note; a present one is also a
note, since the display-only table does start with one.

**Binaries.** PE headers read by hand: machine (anything but AMD64 is an error), subsystem (a `.sys`
must be NATIVE), PE32+, `IMAGE_FILE_DLL`, the link timestamp, the certificate table (is there an
embedded signature), and the CodeView debug id with its PDB path.

**UMD exports.** The export name table of the *built* DLL - not of `bc250umd.def` - must carry
`OpenAdapter`, `OpenAdapter10` and `OpenAdapter10_2`, exactly, undecorated. `driver/umd-stub/bc250umd.c`
has no `__declspec(dllexport)`; the names come from `driver/umd-stub/bc250umd.def`, which
`driver/umd-stub/build.ps1` hands the linker as `/DEF:`. If that switch is ever lost, the DLL builds,
loads, and exports nothing the runtime can find - which in the lab looks exactly like "the runtime never
called us". A missing name is an error; a name present only in a decorated form (`_OpenAdapter@4` from
an x86 `__stdcall` build, `?OpenAdapter@@...` from a C++ one) is an error that says so, because the
runtime does `GetProcAddress` by the plain string. Extra exports are a note.

**UMD load test** (`--load`, off by default). Loads the stub in this Python process with `ctypes` and
calls the three entry points with a zeroed 4 KiB block - both `D3DDDIARG_OPENADAPTER` (`d3dumddi.h`) and
`D3D10DDIARG_OPENADAPTER` (`d3d10umddi.h`) are well under 128 bytes on x64, and the stub ignores its
argument entirely, so nothing can be read past the end. All three must return `E_NOTIMPL`
(`0x80004001`); any other value, or an exception, is an error. The step is deliberately narrow: only a
DLL that `UserModeDriverName` names, only one resolving inside the package directory given on the
command line, only when it is AMD64 and this Python is 64-bit Windows, and `FreeLibrary` afterwards. A
skip is a note, never a pass. This runs our own code on the development PC, which is why it is opt-in;
the stub starts no thread, takes no lock and touches no file (`driver/umd-stub/bc250umd.c`).

It answers a question the export table cannot: that the DLL in *this* package is the stub, and not, say,
a half-finished real UMD that would return `S_OK` with an empty function table and put the runtime a
long way into a stack that is not there.

**Version agreement.** `DriverVer` against `--expect-version`; a three-field expectation accepts the
fourth (build) field, which is `0` for the plain package and `1` for the `-UmdStub` one, the convention
that stops the second package tying with the first on the same hardware id and date.

Then the binary against the INF: `BC250_KMD_VERSION` (`driver/kmd/bc250kmd_escape.h`) is compiled in as
an immediate operand - `0.7.4` is `0x00070004`, returned in `BC250_ESCAPE.Version` - so the tool searches
the **executable sections only** for the little-endian dword. On the real builds this is unambiguous:
`bc250kmd-074` has nine occurrences of `04 00 07 00` and none of `03 00 07 00`, `073` the reverse. When
the expected dword is absent, the tool searches the neighbouring versions (same major, minor within 3,
any revision) and reports which version the image does look like. That is how a 0.7.3 `.sys` inside a
package labelled 0.7.4 is caught. A `.sys` that is not `bc250kmd.sys` and carries no such constant at
all (the older E05 kmdod driver) gets a note instead of an error.

**Catalog.** The member names are read out of the `.cat` (UTF-16LE tags) and compared with the files the
INF installs, which catches a catalog that was not rebuilt after the INF changed. Membership by hash is
`signtool verify /pa /c <cat> <file>`, per file, and its two failures are told apart:

- *"File not found in the specified catalog"* - an error; the file is not in the catalog or its hash
  moved.
- *"...terminated in a root certificate which is not trusted"* - a **note**; the file **is** in the
  catalog and its hash matched, and the chain then failed because the lab certificate is a test
  certificate that this development PC does not trust. Expected.

**Kit tools.** `infverif`, `Inf2Cat` and `signtool` are found under `toolchain\nuget` the way
`driver/kmd/build.ps1` finds them. A missing tool is a note, a tool that rejects the package is an error.
`infverif` runs without a mode switch: `/w`, `/u` and `/h` are the WHQL and Windows Driver rule sets,
which this package fails **on purpose** (dirid 13 without a `TargetOSVersion` decoration, and the stub
DLL going to System32), so those are design decisions and their warnings are notes. `Inf2Cat` can only
work by writing a `.cat`, so it is never given the package: the files are copied to a scratch directory
(under `P:\BC-250\scratch\tmp`, never C:), catalogued there, and only the member list is compared.

## Output files

- `--markdown out.md` - the report for a run's evidence directory: findings table, file table with
  hashes, binary table.
- `--json out.json` - everything the tool established, including the facts the human report summarises
  (INF sections, hardware ids, UMD entries, export lists, catalog members).
- `--manifest out.txt` - a SHA-256 manifest, two `#` comment lines and then `<sha256>  <size>  <name>`
  for every file in the package directory (the whole directory is what gets pushed to unit A). This is
  what freezes the bytes a run measured: a later reader can take the manifest from the evidence and say
  whether a package on disk today is the one that produced the result.

## What is not checked

- **Nothing about the device.** No lab access, no BC-250 needed, no registry, no installed driver store.
- **Whether the driver works.** This tool reads a package; it does not run one.
- **Trust.** The test certificate is deliberately not trusted here, so "would this install with Secure
  Boot on and test signing off" is not answered. That is a property of the lab machine, not the package.
- **The PDB.** Not part of a package and not looked for.
- **The commit.** See below.

## The commit, and BUILDINFO.json

Nothing in a bc250kmd package ties a binary to a commit today. `driver/kmd/build.ps1` runs no `rc.exe`,
so no binary carries a version resource, and no commit string is compiled in. The only build identity in
the `.sys` is its CodeView debug id and the PDB path, which happens to hold the build directory name
(`P:\BC-250\scratch\build\bc250kmd-074\bc250kmd.pdb`) - useful, but it names a directory on this PC, not
a commit.

This matters. `scratch\build\bc250kmd-m7\package-umd` and `scratch\build\bc250kmd-074\package-umd` both
say `DriverVer 09/21/2026,0.7.4.1`, both contain `BC250_KMD_VERSION 0x00070004`, and their `.sys` files
are *different binaries* built 27 seconds apart. Every check in this tool passes on both. Only the PDB
path tells them apart, and only because the build directories were named differently.

So `--expect-commit` is implemented against a `BUILDINFO.json` sidecar, which `build.ps1` does **not**
write yet. If one is present next to the INF, it is checked: `commit` against `--expect-commit`,
`version` against the INF's `DriverVer`, `dirty` (a build from a dirty tree gets a warning), and every
entry of `files` against the actual SHA-256 in the package. The `files` map is the part that matters - a
sidecar whose hashes nobody checks is a sidecar that goes stale, and then it lies about which binary it
is standing next to.

Expected shape:

```json
{
  "commit": "82d39fb",
  "dirty": false,
  "version": "0.7.4",
  "built": "2026-09-21T21:19:25+02:00",
  "package": "umd",
  "files": { "bc250kmd.sys": "<sha256>", "bc250umd.dll": "<sha256>" }
}
```

**The smallest change to `driver/kmd/build.ps1` that would produce it** (reported here, not implemented -
`driver/` is not this tool's to edit): one function and one call per package, after the signing step.

```powershell
function Write-BuildInfo([string]$Dir, [string]$Repo, [string]$Kind) {
    $head = & git -C $Repo rev-parse --short HEAD 2>$null
    $files = @{}
    Get-ChildItem $Dir -File -Include *.sys, *.dll -Recurse |
        ForEach-Object { $files[$_.Name] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() }
    [pscustomobject]@{
        commit  = $head
        dirty   = [bool](& git -C $Repo status --porcelain)
        version = (Select-String -Path "$Repo\driver\kmd\bc250kmd.inf" -Pattern 'DriverVer.*,(\d+\.\d+\.\d+)'
                  ).Matches[0].Groups[1].Value
        built   = (Get-Date).ToString('o')
        package = $Kind
        files   = $files
    } | ConvertTo-Json | Set-Content "$Dir\BUILDINFO.json" -Encoding utf8NoBOM
}
```

called as `Write-BuildInfo $pkg $repo 'plain'` after the plain package is signed and
`Write-BuildInfo $pkgUmd $repo 'umd'` after the UMD one. The file must be written **after** signing (the
signature changes the `.sys` hash) and it is not referenced by the INF, so `Inf2Cat` ignores it and
`pnputil` copies it into the driver store as an inert extra. `packagecheck` already treats a `.json` as a
known extra.

A sidecar is the smallest change, not the strongest one: it can be copied away from its binaries or
edited. The stronger version is a `/DBC250_BUILD_COMMIT=\"$head\"` on the `cl` line and a
`const char Bc250BuildCommit[] = BC250_BUILD_COMMIT;` in the driver, which travels inside the `.sys`
and can be found by the same immediate/string scan this tool already does for the version. That costs a
driver source change and a rebuild of the check, so it is worth doing when the commit question starts
mattering for evidence rather than for convenience.

## Where it belongs in the lab procedure

1. Build (`driver\umd-stub\build.ps1`, then `driver\kmd\build.ps1 -UmdStub ...`).
2. **`packagecheck` on the package that is about to be installed** - the gate. `--model` set to the
   run's model, `--expect-version` to the version the run is supposed to measure, `--load` when the
   package carries the stub. Exit 1: stop, do not touch the lab machine. Exit 2: the tool failed, not
   the package; fix the invocation and run it again, do not skip it.
3. `--markdown` and `--manifest` into the run's evidence directory, next to the install log. Between
   them they say what was installed and what its bytes were.
4. Push and install (`tools\wininstall`), then the run.

Step 2 costs a second and would have saved E16 runs 001 and 002.
