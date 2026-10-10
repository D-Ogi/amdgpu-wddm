/* SPDX-License-Identifier: MIT */
#pragma once
#include "../shim/include/bc250_hip_journal.h"

/* Embedded in a zero-initialized WDDM context. All accesses use the journal's
 * private metadata lock. The referenced process plus monotonic cookie prevent
 * both PID reuse and context-address reuse from granting upload ownership.
 */
typedef struct BC250_HIP_JOURNAL_OWNER {
    LIST_ENTRY Link;
    PEPROCESS Process;
    PVOID Adapter, Context, RuntimeDevice;
    ULONGLONG Cookie;
    ULONG ProcessId;
    BOOLEAN Registered, Enabled;
} BC250_HIP_JOURNAL_OWNER;

NTSTATUS HipJournalOwnerCreate(BC250_HIP_JOURNAL_OWNER* Owner, PVOID Adapter, PVOID Context, PVOID RuntimeDevice);
void HipJournalOwnerDestroy(BC250_HIP_JOURNAL_OWNER* Owner);
NTSTATUS HipJournalEscape(PVOID Adapter, const DXGKARG_ESCAPE* Escape);
NTSTATUS HipJournalBind(const BC250_HIP_JOURNAL_OWNER* Owner, ULONGLONG IbVa, ULONGLONG FenceVa,
                        ULONGLONG FenceValue, ULONG OsFence, ULONG IbBytes, ULONGLONG* Id);
void HipJournalSubmitted(ULONGLONG Id, ULONG Seq, ULONG Vmid, ULONGLONG Root);
void HipJournalComplete(ULONGLONG Id);
void HipJournalFailed(ULONGLONG Id, NTSTATUS Status);
void HipJournalReset(ULONGLONG Id, BOOLEAN Replay);

/* Private dump layout, never a user-mode retrieval ABI. An even Generation and
 * State != WRITING identifies a coherent payload. Pinned states cannot be reused.
 */
#define BC250_HIP_SLOT_EMPTY 0u
#define BC250_HIP_SLOT_WRITING 1u
#define BC250_HIP_SLOT_UPLOADED 2u
#define BC250_HIP_SLOT_BOUND 3u
#define BC250_HIP_SLOT_SUBMITTED 4u
#define BC250_HIP_SLOT_COMPLETED 5u
#define BC250_HIP_SLOT_FAILED 6u
#define BC250_HIP_SLOT_CANCELLED 7u
#define BC250_HIP_SLOT_ABANDONED 8u
typedef struct BC250_HIP_JOURNAL_SLOT {
    volatile LONG64 Generation;
    ULONGLONG Id, ContextCookie, Context, Adapter, Process;
    ULONGLONG Uploaded100ns, Bound100ns, Submitted100ns, Finished100ns;
    ULONGLONG IbVa, FenceVa, FenceValue, Root;
    ULONG State, DataBytes, ProcessId, OsFence, Seq, Vmid, IbBytes;
    NTSTATUS Status;
    BOOLEAN CancelRequested;
    BOOLEAN ReplayPending;
    USHORT Attempts;
    UCHAR Reserved[4];
    UCHAR Data[BC250_HIP_JOURNAL_MAX_BYTES];
} BC250_HIP_JOURNAL_SLOT;
typedef struct BC250_HIP_JOURNAL {
    ULONG Magic, Version, Bytes, SlotBytes, SlotCount, NextSlot;
    ULONGLONG NextId, NextContext, Uploads, Overwritten, Refused, Completed, Cancelled;
    BC250_HIP_JOURNAL_SLOT Slots[BC250_HIP_JOURNAL_SLOTS];
} BC250_HIP_JOURNAL;
extern BC250_HIP_JOURNAL g_HipDispatchJournal;
