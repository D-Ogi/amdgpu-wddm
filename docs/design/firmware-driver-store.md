# Firmware in the Driver Store

The KMD reads its eight firmware binaries from its own signed image directory.
The INF lists each file in `SourceDisksFiles` and `CopyFiles`, with DIRID 13 as the
destination. The names and hashes come from `tools/firmware/cyan_skillfish2.json`.
The public ZIP contains the INF and catalog but omits the firmware binaries.

## Build and installation

The build checks all eight pinned files before Inf2Cat runs. It verifies their
catalog membership, then removes the binaries from the release ZIP. The signed
INF and catalog do not change after this step.

The installer accepts downloaded files or `-FirmwareDir`. It checks sizes and
SHA256 before it copies files into a private package directory. That directory
has a protected ACL for SYSTEM and Administrators. Reparse points are refused.
Open handles prevent replacement of the directory and its ancestors. Read handles
prevent writes and deletion of the package files while staging runs.

Before `DiInstallDriverW`, the installer checks the hashes again, verifies catalog
membership and checks the catalog signer against the package certificate. The
installer writes no successful staging receipt if these checks fail. It removes
its private copies after the call, including on failure.

The retained repair set includes the checked firmware. A repair can run offline.
Legacy repair sets keep their own firmware-folder contract. A malformed new
delivery record cannot select that legacy path. Normal package removal also
removes its Driver Store firmware. It does not claim an older standalone folder.

## Kernel lifetime

At driver entry, `IoGetDriverDirectory(DriverDirectoryImage)` returns the directory
handle. The call requires PASSIVE_LEVEL and a system thread. The handle remains
open until unload, after device workers have stopped. Entry failure also closes it.

Each file open is relative to that handle. The filename comes from the compiled
firmware table. Paths, registry overrides and a fallback directory are not accepted.
Reparse files, empty files, oversized files and short reads fail before firmware
is passed to the PSP. The read limit is 1 MiB per file.

## Validation

`tools/release/test-firmware-package.ps1` uses actual Inf2Cat and catalog APIs.
It checks complete and missing-file packages, changed bytes, offline input, private
directory ACLs, file locks and cleanup. Run it from elevated Windows PowerShell 5.1
with its work and temporary directories outside the repository.

`driver/kmd/test/run_firmware_io.ps1` executes the production reader with fake OS
calls. Faults cover file opens, metadata queries, allocation and reads. The suite
checks handle and buffer cleanup. Its negative control accepts a short read and
must fail the same checks.

Host checks do not establish driver-load or deferred-install acceptance. The
complete BD-152 package still requires the lab install, repair and rollback tests.

## Contracts

WDK and SDK declarations: 10.0.26100.0, `wdm.h`, `mscat.h` and `mssip.h`.
Conceptual documentation revision: MicrosoftDocs/windows-driver-docs
`110f60eaf2ac5836e644d320c1e92c1011f2af5e`.

- [Run from Driver Store](https://learn.microsoft.com/windows-hardware/drivers/develop/run-from-driver-store)
- [IoGetDriverDirectory](https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-iogetdriverdirectory)
- [ZwQueryInformationFile](https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-zwqueryinformationfile)
- [CryptCATGetMemberInfo](https://learn.microsoft.com/windows/win32/api/mscat/nf-mscat-cryptcatgetmemberinfo)

PROVENANCE: linux-firmware, redistributable under LICENSE.amdgpu.
