# packagecheck: package-umd

- package: `P:\BC-250\scratch\build\bc250kmd-07120\package-umd`
- checked: 2026-09-24 07:06:45 UTC
- model: full
- DriverVer: `09/24/2026,0.7.120.1`
- hardware id: `PCI\VEN_1002&DEV_13FE`
- service: `bc250kmd`, binary `%13%\bc250kmd.sys`
- verdict: **PASS** (0 errors, 0 warnings, 14 notes)

## Findings

| level | code | group | finding |
|---|---|---|---|
| ok | INF004 | inf | bc250kmd.inf parsed, 16 sections, utf-8 (no BOM) |
| ok | VER006 | inf | Class Display {4d36e968-e325-11ce-bfc1-08002be10318} |
| ok | VER009 | inf | CatalogFile bc250kmd.cat present |
| ok | VER013 | inf | DriverVer 09/24/2026,0.7.120.1 |
| ok | HW004 | inf | hardware id PCI\VEN_1002&DEV_13FE present |
| ok | SRV009 | inf | ServiceBinary bc250kmd.sys present (dirid 13) |
| ok | CPY008 | inf | bc250kmd.sys copied by [Bc250_Files] to dirid 13, present |
| ok | CPY008 | inf | bc250umd.dll copied by [Bc250_UmdFiles] to dirid 11, present |
| note | FIL001 | files | bc250-lab-test.cer is in the package but not named by the INF (known extra) |
| note | UMD015 | umd | no UserModeDriverNameWow: deliberate, the stub is 64-bit only (bc250kmd.inf explains why a Wow value pointing at the 64-bit DLL would be worse) |
| ok | UMD005 | umd | UserModeDriverName is REG_MULTI_SZ (0x00010000) with 3 name(s): bc250umd.dll, bc250umd.dll, bc250umd.dll |
| ok | UMD012 | umd | user-mode driver bc250umd.dll present |
| note | PE010 | pe | bc250kmd.sys linked 2026-09-24 07:05:33 UTC |
| note | PE013 | pe | bc250kmd.sys has no version resource |
| note | PE014 | pe | bc250kmd.sys debug id 80F0E289-35DF-4E2D-A26B-3F5E3E4FABD7 age 1 |
| note | PE009 | pe | bc250umd.dll has no embedded signature; it is catalog-signed only, which is how driver\kmd\build.ps1 builds it |
| note | PE010 | pe | bc250umd.dll linked 2026-09-21 16:21:29 UTC |
| note | PE013 | pe | bc250umd.dll has no version resource |
| ok | PE003 | pe | bc250kmd.sys is AMD64 PE32+ |
| ok | PE005 | pe | bc250kmd.sys subsystem NATIVE |
| ok | PE003 | pe | bc250umd.dll is AMD64 PE32+ |
| ok | EXP002 | exports | bc250umd.dll exports OpenAdapter |
| ok | EXP002 | exports | bc250umd.dll exports OpenAdapter10 |
| ok | EXP002 | exports | bc250umd.dll exports OpenAdapter10_2 |
| note | VRS004 | version | build field 1: the -UmdStub package convention, so that this package outranks the plain one on the same hardware id and date |
| ok | VRS003 | version | DriverVer 0.7.120.1 matches --expect-version 0.7.120 (build field 1) |
| ok | VRS010 | version | bc250kmd.sys carries BC250_KMD_VERSION 0x00070078 (0.7.120): 13 occurrence(s) in code |
| note | BLD002 | buildinfo | no BUILDINFO.json: this package cannot be tied to a commit |
| note | CAT009 | catalog | bc250kmd.inf is a member of bc250kmd.cat; the chain ends in an untrusted root, which is what a test certificate does on this PC |
| note | CAT009 | catalog | bc250kmd.sys is a member of bc250kmd.cat; the chain ends in an untrusted root, which is what a test certificate does on this PC |
| note | CAT009 | catalog | bc250umd.dll is a member of bc250kmd.cat; the chain ends in an untrusted root, which is what a test certificate does on this PC |
| ok | CAT002 | catalog | bc250kmd.cat names all 3 installed file(s) |
| note | IVF005 | tools | infverif: WARNING(1199) in P:\BC-250\scratch\build\bc250kmd-07120\package-umd\bc250kmd.inf, line 0: The syntax 'DIRID 13 (CopyFiles)' was introduced in OS version 10.0.16299, but DDInstall sections utilizing the syntax will install on earlier OS versions. Those DDInstall sections should be restricted to only install on 10.0.16299 or higher using a TargetOSVersion decoration. |
| ok | IVF003 | tools | infverif: syntax clean, 1 warning(s) |
| ok | I2C007 | tools | Inf2Cat rebuild lists the same members as the shipped catalog |

## Files

| size | SHA-256 | name |
|---:|---|---|
| 792 | `6a967b055def34c084fcf5a71432bce54dbff807d056655b33b5763cccbe6cfd` | `bc250-lab-test.cer` |
| 2839 | `f10f57b6e3556bac0af23d58ac2570f5b94aeabdb6eb5e8cbe155b89f0881367` | `bc250kmd.cat` |
| 7603 | `61cada0880beeb198aaeeba1a57588533017a7a932161b341e3dbf1f6411128f` | `bc250kmd.inf` |
| 347536 | `f1319389c30c10fb59bd5228780fe5924212dfef587ba054d2fb519dd7034774` | `bc250kmd.sys` |
| 4096 | `d5bd1ad82c00438900dc47a50b3efa36d98f9a9cc7f6987d0f1545a0de4cb661` | `bc250umd.dll` |

## Binaries

| file | machine | subsystem | linked (UTC) | signed | version resource |
|---|---|---|---|---|---|
| `bc250kmd.sys` | AMD64 | NATIVE | 2026-09-24 07:05:33 | yes | none |
| `bc250umd.dll` | AMD64 | WINDOWS_GUI | 2026-09-21 16:21:29 | no | none |

`bc250umd.dll` exports: `OpenAdapter`, `OpenAdapter10`, `OpenAdapter10_2`
