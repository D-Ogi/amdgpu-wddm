# User-mode files in the driver package

The INF owns the installed graphics binaries and their adapter registry entries.
The control application and command-line tools remain in Program Files.
Windows assigns a new DriverStore directory when the package changes.

## Layout and registration

`tools/release/driver_package.py` maps the pinned release inputs to `payload/kmd` before catalog generation.
The source and destination subdirectories match.
Every INF basename is unique.
The x86 names end in `32` and stay below `wow64`.

| Directory below the package root | Contents |
|---|---|
| `desktop` | Router, CPU UMD, hosted UMD and its RADV backend |
| `d3d11` | D3D11 shell, DXVK backend, RADV backend and configuration |
| `d3d12` | D3D12 shell, vkd3d backend and RADV backend |
| `vulkan` | Vulkan ICD and a manifest with a relative DLL path |
| `opengl` | OpenGL ICD for each declared architecture |
| `mft` | Video encoder MFT when the release includes it |
| `wow64` | The declared x86 counterparts |

The INF uses DIRID 13 for all these files.
Its AddReg entries set the D3D, Vulkan and OpenGL paths through `%13%`.
The D3D9 slot remains `<>` for D3D9On12.
Readers skip that slot before processing file paths.
`AmdgpuDriverPackage` identifies the bound package directory.
The GUI also checks that the service image belongs to that directory.

The manifest declares the available architectures.
An absent x86 OpenGL ICD does not acquire an INF registration.
The release catalog covers each declared driver file.

## Loading and configuration

The router and shells resolve their dependencies from their own module directories.
The shared names are in `driver/contract/amdgpu_driver_package.h`.
Production routers ignore the old registry overrides for library paths.
Only the private-hive test router enables those overrides.
Route selection still follows the installed policy.

D3D11 configuration version 3 keeps the version 2 layout and capability fields.
Its backend hashes are SHA-256 image digests from the Windows catalog API.
These digests exclude the embedded signature.
Replacing a signature therefore does not invalidate the backend identity.
Versions 1 and 2 retain their original whole-file hash interpretation.
The loader holds the verified files against writes and deletion while loading them.

Packaging first checks the original input hashes and configuration.
It then converts the configuration without changing the declared capabilities.
Stamp VERSIONINFO before creating the input configuration and release hash pins.
The stamp changes the image digest, so it must precede the conversion to version 3.
The provenance gate reconstructs this conversion and rejects other edits.
Signature trust is a separate package check.
An image hash does not identify a trusted signer.

## Upgrade and rollback

The installer stages the complete package through PnP.
It verifies the bound package, registry types and payload hashes before retiring legacy files.
The retirement plan survives the required restart.
Only old files with recorded, matching hashes are removed.
The installer does not remove DriverStore members itself.

Migration captures and removes the old library overrides and owned Khronos ICD entries.
It leaves other vendors' entries intact.
Rollback restores only checked paths inside the previous installation root.
The previous package and its manifest remain part of the installer transaction.

## Submission and signing

Pass `-SubmissionCab <path>` and `-KmdPdb <path>` to `build-release.ps1`.
The CAB contains one `driver` folder and no root files.
The PDB must match the SYS CodeView GUID and age.
The builder expands the CAB and verifies every file and catalog member.
The CAB includes the pinned firmware required by the INF.
The public installer ZIP retains the separate firmware download path.
The owner signs the CAB with EV before Partner Center submission.

The default mock-attestation step signs each driver PE and regenerates the catalog.
It verifies unchanged image identities before updating the manifest's whole-file hashes.
The manifest records this as a test signing rehearsal, not Microsoft attestation.
`-ExpectedSigningRoot pinned-test` verifies the signature, image digest, code-signing EKU and exact pinned chain root.
It permits only the untrusted-root chain error and changes no certificate store.
`-ExpectedSigningRoot system` uses `signtool verify /pa`.
The mode is explicit and has no fallback.

## Release and lab packages

`build-release.ps1 -Flavour release` is the default.
`-Flavour lab` appends `-lab` to the package and installer filenames.
The product version stays the same. A lab build refuses `-SubmissionCab`.
Both packages retain embedded test signatures and catalog verification.

Source schema 4 assigns each `files[]` row a `flavour`: `release`, `lab` or `shared`.
Both flavours must provide every payload slot.
PE images and loader configurations require separate rows for each flavour.
Only data files can use `shared`. Duplicate or incomplete selections fail before staging.
Legacy source lists are valid only for the release selection.
The selected pins also drive the rebuild and provenance checks.

Each flavour derives its v3 loader configuration from its own stamped backends.
The manifest and DriverStore report record the selected flavour.
This label does not prove that diagnostic code is absent.
The component build gates must check that release images omit lab interfaces
and that lab images contain them. HLK uses the release flavour.

## Validation

Run the package, signature, router and engine-loader host gates before integration.
The generated INF must pass InfVerif `/w` and `/h`.
The catalog must cover every installed driver file.
Run the private staging ACL gate with an elevated Windows token.

On the lab, test clean install, upgrade, rollback and uninstall.
Check DWM, the native graphics APIs and each shipped x86 path.
Check module paths against the bound DriverStore directory.
Run HLK against that exact integrated artifact.
Host gate results alone do not establish HLK compliance or game compatibility.

## Contracts

The local Microsoft documentation revision is `110f60eaf2ac5836e644d320c1e92c1011f2af5e`.
The package uses WDK `10.0.26100.0`.

- [WDDM 2.1 package requirements](https://learn.microsoft.com/windows-hardware/drivers/display/wddm-2-1-features)
- [Run from DriverStore](https://learn.microsoft.com/windows-hardware/drivers/develop/run-from-driver-store)
- [Attestation submission](https://learn.microsoft.com/windows-hardware/drivers/dashboard/code-signing-attestation)

