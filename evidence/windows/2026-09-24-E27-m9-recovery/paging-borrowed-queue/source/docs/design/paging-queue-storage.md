# Paging queue storage owned by DMA buffers

Status: M425 source/host queue integration; runtime builders still emit legacy records.
Installed unit A remains M423 KMD0.7.132.1. The full M9 resource gate stays open.

## Contract and problem

Local Microsoft DDI snapshot7515063c, enriched WDK26100 declarations:
[DXGK_CONTEXTINFO](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_contextinfo)
says DMA private storage is allocated from nonpaged pool, initialized when the
DMA buffer is created, and not accessed by VidMm during that buffer's lifetime.
[BuildPagingBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_buildpagingbuffer)
provides the remaining private storage and allows advancing its output pointer.
[SubmitCommandVirtual](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_submitcommandvirtual)
provides the driver's private buffer; malformed-command rejection is distinct
from a valid submission needing more kernel pool memory.

WddmSubmitPagingHardware now borrows the prebuilt slot for queued records, but
all current builders still emit legacy records. Their submissions still allocate
a nonpaged job plus a private metadata copy, including native transfers without
a captured-page graph. The resource gap remains until builders migrate.

Local conceptual snapshot110f60ea, system-paging-process.md, documents the1GiB
paging VA layout and chunking larger allocations. threading-and-synchronization-
first-level.md lists BuildPagingBuffer among nonreentrant scheduler DDIs. This
serializes a call, not every unfinished operation across multiple calls. The
legacy TransferStart/TransferEnd guarantee must not be silently assumed to
provide a numeric bound for virtual transfer captures; TransferVirtual flags
only describe source/destination page sizes. Keep capture reservation/fallback
until its own resource proof or replacement is complete.

## Implemented private format

Legacy direct kind0 and native kind1 remain unchanged. New kind2 contains the
same direct header/payload plus queue storage; kind3 contains the same native
metadata plus queue storage. Walkers normalize them to direct/native spans and
never expose queue bytes as SDMA commands.

Each queued record begins on an8-byte CPU boundary. The queue region begins at
an8-byte boundary after immutable command data/metadata. Each job slot is64bytes:

- Direct: one slot for each possible DWORD start in that record. Disjoint partial
  submissions therefore get different slots even when they share a record.
- Native: one slot, because the existing parser only admits whole native spans.

A single slot for the entire DMA buffer would be insufficient for simultaneously
pending partial submissions. Slot selection validates the entire requested
range before returning writable storage. A request spanning several records uses
the slot corresponding to its first selected command. Different OS buffers
remain independent even when GPU VA values coincide.

Private size is align8(24 + commandBytes) +64*(commandBytes/4) for direct records,
and128bytes for native records. PagingPrivateQueuedDirectCapacity computes the
largest payload fitting the supplied remaining private bytes; callers must also
apply existing DMA/live-ring limits. Constructors clear only padding/job storage,
preserve direct command words, and refuse before writing when storage is short.
This trades private-buffer capacity for guaranteed per-submission job space;
it does not impose an arbitrary global queue length or block the submit DDI.

## Required runtime integration

1. Make every supported builder reserve queued private bytes before encoding
   or logical page-table publication. Advance private pointers by the new size.
   Update partial-buffer tests against actual DMA and private budgets together.
2. Give BC250_PAGING_JOB a data pointer and ownership state fitting64bytes
   (compile-time assertion). Under the existing lock, claim the selected OS slot
   and queue immutable record data without allocating/copying another buffer.
   Define duplicate/replayed/overlapping submission ownership explicitly.
3. Retire/unlink/clear borrowed slot ownership BEFORE publishing completion that
   lets Windows reuse the DMA buffer. Do not touch borrowed memory afterwards.
   A timeout must retain ownership, not report fake completion or reclaim a slot.
4. Cover stop/cancellation/preemption and device generation. Detach borrowed
   records while their OS lifetime is still valid; do not free OS-owned memory.
   Distinguish legacy heap-backed records during migration. No hidden heap
   fallback may be counted as complete removal of submit-time allocation.
5. Host-test interleaved partial ranges, independent buffers, immediate and late
   fences, timeout retention and completion/reuse ordering. Then bump the KMD
   revision, deploy once through PnP and repeat positive shader/model/residency
   controls with allocation/queue ownership witnesses.

M424 introduced the portable format only. M425 implements the queued admission
and retirement code while preserving an explicit legacy heap path. Builders
have not migrated, so the installed path still depends on pool allocation.
The source/host checks do not prove OS callback lifetime.
See [source, tests and limits](../../evidence/windows/2026-09-24-E27-m9-recovery/paging-queue-storage/RESULT.md).

## M425 - Borrowed queue nodes

The actual BC250_PAGING_JOB structure fits a64-byte slot, checked at compile
time. It points to immutable command records instead of embedding an inline
copy. Admission claims a queued slot under the existing lock and refuses a
still-owned start without overwriting its fence or FIFO links. Independent
split starts and independent buffers can wait together. Legacy and zero-byte
records retain explicit heap allocation during migration.

On a real fence, retirement detaches the node, caches its needed fields and
clears borrowed ownership before WddmRecordCompletionLocked. No borrowed
pointer is read or freed after publication. A timeout keeps the pending node;
a later real fence permits retirement. The existing stop drain distinguishes
borrowed storage from heap storage. Its hardware quiescence and OS lifetime
preconditions remain separate acceptance gates.

The host harness extracts the actual structure, submit/fence/watchdog functions
and stop-drain block, and links the actual private-record parser.590 checks pass
for legacy copies, interleaved splits, equal VAs in different buffers, native
records, immediate/late completion, timeout retention and slot reuse. Completion
poisons the retired slot to model immediate OS reuse. A copied fixture that
publishes completion before release fails91 checks. These are serialized host
interleavings with OS/GPU mocks, not a threaded or hardware proof.

Full WDK build and875868 actual-builder regressions pass. No lab deployment.
Next migrate every builder's private capacity, header publication and pointer
advance together; ensure queued alignment across mixed records. Then test
actual builder-to-queue integration and remove or explicitly justify remaining
legacy/zero-byte allocation before claiming the submission resource gate closed.
Evidence: [M425](../../evidence/windows/2026-09-24-E27-m9-recovery/paging-borrowed-queue/RESULT.md).
