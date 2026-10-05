# Deferred installation rejected; exact166 recovered

Unit A,2026-09-28. Runnerd3548e4, stage005 manifest
A25262AEACB6A7CB70C77D9D602C23533799876203CBA88B98EE7403DBC51355.
CPU-only166->169->166 rehearsal;180s task cap. Supervisor ended22.9281786s with
restore-unverified. Both DIF_INSTALLDEVICE calls returned3758096919 (E0000217),
ERROR_BAD_SERVICE_INSTALLSECT in SDK26100 SetupAPI.h. Both selected exactly one
expected-version node with disabled problem22 before the call. No deferred install
was accepted. This does not test KMD169's VSync fix.

The disabled device retained exact166 SYS but initially lacked a readable driver
version property. Explicit newdev rebind restored166.1; disable/configure/enable
then reported problem0, but independent health still returnedC000000E and DWM had
no CPU module. Never interpret that problem0 alone as successful recovery.

After preserving diagnostics, explicit recovery reset the two-start guard and
requested an orderly restart with saved configuration. That guard reset was not
health confirmation. New boot02:44:58.5Z still reported43/no interface. A direct
registry read found no UMD/ICD values in the bound class key. Restoring them with
RegistryKey plus disable/enable produced exact166 health7 and CPU UMD8279 in
DWM2652. Later health15 was observed after71.7s ready; the explicit confirm script
expected7 and therefore did not perform another confirmation. Do not attribute
that transition to that script. Final preflight02:53Z passes,67.1C. No AC cycle.

Task removed, no matching169 INF package remaining. Full local receipts, SetupAPI
log and recovery scripts stay under scratch/g0-hosted/kmd169-stage005-ops. The
preserved SetupAPI device log did not contain the rejected calls' E0000217 text;
it is not evidence of their deeper cause. Raw selected trial receipts are copied
here; the final summary excludes private machine/device identifiers.

Open: explain service-section rejection before another attempt; include the60s
ready-age and15s completion-freshness gates in confirmation budgeting; verify
registration by readback and modules during recovery. GPU desktop G0 stays open.
