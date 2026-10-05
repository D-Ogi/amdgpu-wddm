# M434: Native SMU owner, concurrent transaction/stop control

Date: 2026-09-24. Source and host validation only. No lab command or deployment.
Previous turn M433 was progress: tested transport and owner feedback evidence.
The full M9 goal remains open.

## Changes

The KMD now compiles `smu.c` with the actual M429 policy and M433 transport.
An EX_PUSH_LOCK protects the entire clock transaction at PASSIVE_LEVEL with
normal kernel APCs disabled by a critical region. The caller thread is tracked;
callbacks reject another thread. Stop acquires the same lock, marks the owner
offline and clears its borrowed register mapping before releasing the lock.
Later calls refuse before IO. The device lifecycle must retain the containing
object through all calls, join users and invoke Stop before BAR unmap.

Native callbacks use READ/WRITE_REGISTER_ULONG and generated named MP1 offsets.
KeQueryInterruptTimePrecise has a required output pointer in WDK 26100; local
contract review found and fixed the initial NULL argument before deployment.
Polling is bounded at 20 ms per prior/current response and uses one-us stalls.
Warm initialization always polls the old response before any new command.
Only fixed1000MHz/820mV preparation and paired readback APIs are exposed; no
unrestricted message API or independently adjustable frequency/voltage entry.

The candidate THM callback uses AMD THM_TCON_CUR_TMP fields and regcalc offsets.
It has NOT been validated through the GPU BAR on unit A. The existing measured
sensor uses SMN via the host bridge. Same offsets do not prove BAR accessibility;
L36 records the required comparison. Do not activate this backend before it.

New bc250rd source has no mailbox mapping, write routine or send implementation.
Its legacy SMU IOCTL explicitly returns NOT_SUPPORTED, including PnP gaps.
The old deployed artifact remains intact. Replacement KMD clients/startup are
not wired yet, so this new reader must not be deployed on its own.

## Controls

- Actual native owner source compiled with Win32 SRW locks standing in for kernel
  push locks: four threads each perform50 preparations and50 paired reads.
  All1200 mailbox commands complete with correct frequency/VID and model67C.
- Deterministic held command plus concurrent Stop: Stop remains blocked until
  the entire four-command preparation finishes; later calls return not-ready
  with zeroed outputs. Same-object restart/read succeeds.
- 23135 checks, zero failures. This tests real host concurrency and source
  sequencing; it does not prove the Windows kernel scheduler/IRQL behavior.
- Stop mutation removes the join lock: four failures, including early Stop
  completion and interrupted preparation. Source and output preserved.
- M433 integrated protocol regression:607 checks, zero failures.
- Full WDK KMD compile/link/sign and new bc250rd compile/link/sign pass.
- Initial host mock failed because WIN32_NO_STATUS suppressed ntstatus.h and
  the mock lacked MAXULONG/realistic IRQL accessor; corrected model then passes.
  All initial logs preserved. Native WDK build was independently successful.

Commands: `driver/shim/test/run_smu_native.ps1`, `run_smu.ps1`,
`driver/kmd/build.ps1 -Kits <workspace>/toolchain/nuget -Out <fresh directory>`,
`tools/win/bc250rd/build.ps1` with the same Kits/Out convention. Build logs and
artifact SHA256 identify development outputs. These are never installed as134.

## Remaining integration

No BC250_DEVICE field, PnP activation, escape/client routing or early-start hook
is installed by this change. Implement those as one handover: migrate the CLI
and overlay, prove no loaded legacy writer remains, verify native temperature,
then clock preparation before GART/PSP/GFX. Preserve a deliberate rollback pair;
never silently restore direct mailbox access when KMD is unavailable. Retain the
old underclock task until the new startup path is tested. PnP and OS-boot trials
are distinct acceptance gates. No firmware writes are involved.

Raw logs and source snapshots contain no private data. No hardware claims are
inferred from these model controls or build success.
