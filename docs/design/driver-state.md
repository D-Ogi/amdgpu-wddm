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
Two readers accept a generation of the same schema under an earlier catalog as the source of an update in place:
the installer worker (settings snapshot, prepare) and the first start of the new package with a pending handoff.
Both read it by setting name, write no value into it and bind no receipt from it. The transaction publishes the
new generation under the new catalog. Without a handoff the KMD refuses such a generation.
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

### Transaction phases of the installer

The installer record `driver_state` in `state.json` has one of three phases (BD-298).
One table in `tools/release/installer/state-lifecycle.ps1` gives the step for each phase and intent.

| Phase | Install of the same package | Install of another package | Acknowledgment | Uninstall |
|---|---|---|---|---|
| no record | new transaction | new transaction | refused | removal |
| `prepared` | the same transaction again | abandon, then a new transaction | `acknowledged` | abandon, then removal |
| `acknowledged` | new transaction | new transaction | the same check again | removal |
| `abandoned` | new transaction | new transaction | refused | removal |

"The same package" means the same product version, device and INF hash.
An install can stop after its prepare and before its acknowledgment, for example after a power loss or a blocked program.
The worker step `abandon <transaction> <product> <device>` then removes the hold of that transaction. The step holds the writer lock:

1. A freeze of another transaction stops the step before it changes anything.
2. The `pending.bin` of this transaction moves aside as `retired-<transaction>-pending.bin`. A file of another state catalog is accepted, because the stopped install can be of another release.
3. The step releases the freeze of this transaction.

A store whose first transaction did not finish needs one more rule.
The KMD creates `StateV2` and `Generations\<transaction>` at the start of a transaction, and it writes `Active` only at the end.
If a start of the KMD stops between these points, the store has no `Active` value, no installed generation and no freeze.
The bound package already declares the state catalog.
For such a store, the abandon step only moves the file.
The prepare, the archive of an uninstall and the settings read then use the legacy state, which was the source of the unfinished transaction.
The KMD accepts a legacy transfer into such a store, because no `Active` value and no earlier generation exist.
In all other cases, a bound package that declares the catalog without a store with `Active` stays an error.
The archive of an uninstall of such a store has origin 2 (legacy).
The worker writes that archive only as `archive-<transaction>.bin`, and not as a removal record.
The reason is that the prepare of every released install (0.7.216.106 and later) accepts only a removal record of an installed generation.
On a removal record of another origin, the prepare stops with error 50, `ERROR_NOT_SUPPORTED` (lab 26200, tester.37).
A removal record of this transaction and a legacy removal record move aside as `retired-<transaction>-<name>`.
The removal record of an earlier uninstall of an installed generation stays, because it holds the last settings that were active.
An install after this uninstall reads that record, or installs fresh when there is none. The settings of the unfinished store never became active.
A resumed uninstall of that store reads its own `archive-<transaction>.bin` and accepts only a legacy archive of its transaction, device and product.
Main 967c6d76 wrote such a legacy removal record. If an install finds one of its device, the record moves aside as evidence and the install continues fresh.
The installer then writes a warning: the settings are not carried over, because the install of that version did not finish.
If the KMD has already applied the handoff, `Active` names the transaction and the store has no freeze: only the file moves.
The same transaction can publish its handoff again after an abandon. A retired copy with the same bytes then completes the step.
The worker removes `pending.bin` only after a byte compare under the lock, and the installer log records this removal.
A `pending.bin` of another transaction, a damaged file, an incorrect ACL and a retired copy with other bytes stop the step. The step keeps the files in each of these cases.
The step does not need the device. A retry does only the work that remains.
The installer then writes the phase `abandoned`, keeps a copy of the record in `driver_state_history` and writes a warning line.
`abandoned` is terminal for its transaction: no step resumes or acknowledges it.
The uninstaller of a package uses the worker of that package for the abandon step.
The installed worker can be of a release without that step.
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
