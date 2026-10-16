# Driver state and migration

The driver, router, control application and tools share one setting contract.
`driver/state/catalog.json` owns the types, ranges, defaults, access rules and migration rules.
Its generator produces the C catalog, INF defaults and `amdgpu-state.bin`.
The package gate rejects a catalog mismatch between the binary file, CLI and installer worker.

## Storage and reads

The INF owns installation defaults under `DefaultsV2` in the driver's Parameters key.
The KMD opens that key through `IoOpenDriverRegistryKey` with `DriverRegKeyParameters`.
It uses `DriverRegKeySharedPersistentState` for mutable state.
User-mode clients obtain that same root through `GetSharedServiceRegistryStateKey`.
Neither runtime adapter reconstructs a service registry path.

At driver entry, the KMD adds Builtin Users read access to the shared root.
This explicit ACE does not inherit. It preserves the OS ACEs, owner and DACL protection.
Public child keys already allow these readers. Private migration and wave records keep their own protected ACLs.
The repair also runs for an existing generation, before state activation. A failed repair prevents initialization.
It grants no write access or permission to remove keys or change permissions. Explicit deny entries remain effective.

`StateV2` holds an active generation, its data, and separate volatile records.
A generation has a schema and catalog identity. Readers reject incomplete or unknown generations.
User-mode readers check the signed package's catalog file before they consume factory defaults.
This path also supports diagnosis when the graphics adapter cannot start.

Effective setting reads check the user override, installation default and code default in order.
Only an absent value permits the next source. Access errors, invalid types and failed reads remain errors.
Raw reads preserve absence for backups and migration. Runtime records never acquire setting defaults.
Invalid CPU, CU, DPM or fan settings close the affected control group.

The native client exports the same reader to the GUI and overlay.
Its batch API holds the writer mutex from backup through writes, checks and rollback.
The installer freeze waits for that mutex. A stale generation cannot accept writes.
A batch does not promise crash atomicity or stop kernel writes.

## Migration and removal

The SYSTEM installer worker captures the old state before installation can replace its defaults.
It binds the transfer to the device, transaction, package version and catalog.
Protected files under DriverData retain the transfer. Reparse points and unexpected ownership fail the operation.
The worker freezes new-schema user-mode writes. The final capture also requires a stopped driver and disabled device.

Driver entry checks the candidate without publishing it. Device attachment checks the actual device identity before activation.
The transaction writes and flushes a complete generation before it changes `Active`.
A failed or interrupted transaction cannot publish a partial generation.
Legacy absence follows the catalog's migration rule, including explicit zero for `CpuTune`.

Acknowledgment requires the installed generation to match the transaction. The worker archives the transfer before it removes the pending file.
Uninstall requires a durable archive before it removes service state.
Repair preserves runtime closure records. Recovery actions in the GUI remain separate, explicit operations.

Board memory backup and journal files have separate protected storage.
First initialization consumes a durable authorization before it creates them.
A missing file after that point cannot silently become a new original backup.
Unknown or incomplete board provenance disables board writes.

## Explicit legacy rollback

`install.cmd -RollbackPackage <folder>` uses the new installer to restore a compatible old package.
The target must declare `INSTALL_AS_SET` and the supported legacy KMD ABI.
The worker captures the source identity and state while the device is stopped and disabled.
It removes only the captured source INF. Other competing package ranks cause refusal.

The target INF, SYS and manifest hashes remain part of the protected receipt.
After deferred installation, the worker checks the target binding before it restores and reads back the legacy values.
Only then can it retire the frozen generation and enable the device.
The converter requires a complete board backup pair with a clear journal.
Missing, unknown or pending board state causes refusal. The device remains disabled after a failed conversion.

## Validation boundary

Host gates cover the resolver, malformed transfers, interrupted publication, protected files and installer continuation.
Native x86 and x64 fixtures check generation changes, batch ownership, thread identity and migration freeze.
The lifecycle fixture executes the root ACL repair and checks read and write access with Windows Authz.
GUI fixtures execute backup, apply and rollback without opening a window.
The KMD build and source INF checks are separate gates.

These checks do not constitute Driver Isolation or HLK acceptance.
The candidate still needs fresh install, upgrade, repair, uninstall and rollback on a disposable Windows installation.
Restricted-token access, both desktop routes and the installed signed package must also pass there.
The firmware staging contract is described in [Firmware in the Driver Store](firmware-driver-store.md).

## Contract sources

WDK and SDK declarations: 10.0.26100.0, `wdm.h` and `winsvc.h`.
Conceptual documentation: MicrosoftDocs/windows-driver-docs revision `110f60eaf2ac5836e644d320c1e92c1011f2af5e`.

- [Driver package isolation](https://learn.microsoft.com/windows-hardware/drivers/develop/driver-isolation)
- [IoOpenDriverRegistryKey](https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-ioopendriverregistrykey)
- [GetSharedServiceRegistryStateKey](https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-getsharedserviceregistrystatekey)
