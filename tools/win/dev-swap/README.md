# dev-swap

The development file swap. It puts new user-mode driver files of a rebuild into the active driver package of the lab unit, and puts the installed files back. A fix gets to the lab without a release set and without a package.

The swap changes only the user-mode files of the active `bc250kmd` package in the DriverStore. It does not run pnputil. It does not restart the device or the operating system. It does not change the kernel driver. When a running process holds a file, the swap renames that file aside to `<name>.swap-aside-<utc>` and writes the new file under the old name. The process keeps the old image until it ends, and a new process loads the new file.

## Commands

Run the commands on the development PC from the repository root, with `BC250_ROOT` set. The lab is reached only through `tools/win/target.py`.

```
python tools/win/dev-swap/dev_swap.py plan    --file x64:dxvk=<path> --file x86:dxvk=<path>
python tools/win/dev-swap/dev_swap.py swap    --file x64:dxvk=<path> --file x86:dxvk=<path> [--package bc250kmd.inf_<id>]
python tools/win/dev-swap/dev_swap.py status
python tools/win/dev-swap/dev_swap.py restore [--drop-stale]
```

- `plan` checks the files and shows the records that the swap writes. It does not call the lab.
- `swap` reads the lab state, writes the records, pushes the files and swaps them. A swap on top of an earlier swap is one step. Do not restore between two swaps.
- `status` shows each role as `installed` or `swapped`, with its SHA-256. It also shows whether each D3D11 record pins the active engine and ICD. It never refuses.
- `restore` puts back the installed bytes of each changed role and proves each one by its SHA-256. Then it deletes the state.

Each step prints one verdict line: `<STEP> PASS|FAIL|REFUSED <reason>`. The exit code is 0 for PASS, 1 for FAIL and 2 for REFUSED. The tool masks the lab addresses in its output.

## Roles

`--file` takes `<arch>:<role>=<path>`. The architecture is `x64` or `x86`, where `x86` is the WoW64 lane. A role that has no `--file` stays as it is installed.

| Role | Installed name (x64, the x86 name adds `32` to the stem) | Identified by |
|---|---|---|
| `d3d11-shell` (or `d3d11`) | `amdgpu_wddm_d3d11.dll` | export `OpenAdapter10_2`, OriginalFilename |
| `dxvk` | `amdgpu_wddm_dxvk.dll` | export `Bc250DxvkEngineGetFuncs`, OriginalFilename |
| `d3d11-icd` | `amdgpu_wddm_radv_d3d11.dll` | export `vk_icdGetInstanceProcAddr`, OriginalFilename `amdgpu_wddm_radv.dll` |
| `d3d12-shell` (or `d3d12`) | `amdgpu_wddm_d3d12.dll` | export `OpenAdapter12`, OriginalFilename |
| `vkd3d` | `amdgpu_wddm_vkd3d.dll` | export `Bc250Vkd3dEngineGetFuncs`, OriginalFilename |
| `d3d12-icd` | `amdgpu_wddm_radv_d3d12.dll` | export `vk_icdGetInstanceProcAddr`, OriginalFilename `amdgpu_wddm_radv.dll` |
| `icd` | both ICD roles of the architecture | as above |
| `d3d11-record` | `amdgpu_wddm_d3d11.config` | never an input: the tool writes it |

The names come from `tools/release/driver_package.py`, so the tool and the package use one list.

## The D3D11 record

The D3D11 shell loads the engine and the ICD only when their image digests agree with the pins in its record. The record is `amdgpu_wddm_d3d11.config`, version 3, 132 bytes. The engine pin is at bytes 68 to 99 and the ICD pin at bytes 100 to 131. If an engine changes and its record does not, `D3D11CreateDevice` fails with `ERROR_CRC`.

When the engine or the ICD of an architecture changes, the tool writes the record of that architecture:

1. Positive control: the installed record must be a valid version 3 record that pins the installed engine and the installed ICD. If it does not, the swap refuses and writes nothing.
2. The new pins are the image digests of the engine and the ICD that are active after the swap (new or installed). The capability bytes do not change. `versionstamp.repoint_m14c` writes the pins.
3. The lab script does the same control again before its first write, with digests that it calculates on the lab. It also makes sure that the new record pins the files after the swap.

The image digest is the Windows catalog hash, `CryptCATAdminCalcHashFromFileHandle2` with SHA-256. This is the hash that the shell checks, and `tools/release/image_identity.py` calculates the same value on the host.

The D3D12 shell has no file pin for its engine. It checks the engine ABI when it loads the engine.

## ABI versions

The binaries do not carry an ABI version as data. Each engine returns its ABI version from its `GetFuncs` export, and the shell checks it at the first device open. The tool checks the export. If the versions are not compatible, the first device open fails. In that case, use `restore`, or swap an engine and a shell of the same build.

## Refusals

The swap refuses, with one line, and writes nothing, when:

- a file is not a PE image, or is of the wrong architecture
- a file is of the wrong role (export set or OriginalFilename)
- a file has no version resource: it is identified by its export only, and `plan` says so
- the lab does not run the expected package (`--package`), or the service and the adapters do not agree about the active package
- the HLK campaign guard service runs, or the guard has a trip. The lab script reads the guard files with shared access. It never calls `guard.ps1 -Action Status`.
- the record control fails (see above)
- the lab state belongs to another package. `restore --drop-stale` deletes that state.

## State

The state is on the lab, in `C:\BC250\dev-swap\state`: `state.json` (package, roles, the SHA-256 of each original) and `orig\` with the original bytes. The first swap keeps the installed bytes of each role that it changes. A later swap keeps only the roles that are not in the state yet. Thus restore always puts back the bytes that were installed before the first swap. Restore works from a new host session and after a lab restart. It does not need a pins file on the host.

## Tests

`python -B -m unittest discover -s tools/win/dev-swap -p "test_*.py"` (the `dev-swap` gate of `tools/quality/quick.ps1`). The tests run the lab script in Windows PowerShell 5.1 with `-FakeRoot` on a fake DriverStore tree. They cover swap, double swap, restore, the record writer, a held file and the refusals. They also compare the digest of the lab script with `image_identity` on real PE images, and do a parse check of the lab script.
