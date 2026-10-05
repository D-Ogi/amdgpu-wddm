# CP1 diagnostic checkpoints (M316)

Hypothesis: synchronous pre-operation checkpoints can distinguish wrapper lock acquisition, MQD preparation and KIQ register setup without changing the AMD register sequence. This change prepares the next hardware experiment; it does not identify the existing hang.

Changes: optional synchronous callback in the CP step helper. Cold KIQ reports scheduler, clear-mqd, select-queue, build-mqd, disable-wptr-poll, read-active, program-hqd, activate-hqd, restore-selection, complete. The active-queue branch has an additional recovery checkpoint. Each label precedes its operation; complete follows selection restore even when register setup returns an error (the caller still checks the result).

Only the unpublished traced CP1 path acquires GartLock with ExAcquireFastMutexUnsafe, paired with ExReleaseFastMutexUnsafe inside the existing critical region. It keeps the same mutex and GfxPagingLock order. PASSIVE_LEVEL and special APC delivery are required; other paths keep their existing mutex operations. Wrapper checkpoints bracket both lock acquisitions. No lock release or extra MMIO is inserted between the AMD operations. The callback does not call back into GPU operations. Keeping filesystem I/O under these locks deliberately increases diagnostic start latency; it is not a performance mode. Existing KeepLog opt-in controls entry to CP substeps.

Microsoft contracts consulted 2026-09-23:
- [ExAcquireFastMutexUnsafe](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exacquirefastmutexunsafe): remarks explicitly permit prior KeEnterCriticalRegion and do not raise IRQL. The requirements table says APC_LEVEL; the documented critical-region alternative is used here.
- [ExReleaseFastMutexUnsafe](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exreleasefastmutexunsafe): pair with Unsafe acquisition.
- [ZwCreateFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatefile): PASSIVE_LEVEL and special APCs enabled.
- [Critical regions](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/critical-regions-and-guarded-regions): normal APCs disabled, special APCs remain enabled.

Validation: WDK build passes. `run_gfx.ps1 -Cp1Checkpoints` and default both exit0 with exact Linux trace match354+35 writes,24 known address exceptions,11 stub ring tests and four discriminating controls. Traced cold runs each report10 checkpoints/zero ordering errors. The trace backend does not model Windows mutexes, filesystem reentrancy or I/O timing. Build success does not establish those runtime properties, nor hang resolution.

Development SYS hash in hashes.json; no deployment and no new hardware initialization. Installed lab driver remains07101 with all hardware gates closed following M315. The development image also contains the undeployed M289-M310 paging work; do not represent it as an instrumentation-only candidate. A versioned package and startup/lock review are required before the next trial.
