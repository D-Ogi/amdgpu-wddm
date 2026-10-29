/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// The FSR 4 identity answer: the kernel binding (amd_identity.c). The policy is in amd_identity_policy.h.
#pragma once
#include "amd_identity_policy.h"

struct _BC250_ESCAPE_PROCESS_POLICY;

// The identity record of one KMD process object (wddm.c, BC250_WDDM_OBJECT.AmdIdentity). ExAllocatePool2 zeroes it,
// which is "no name, nothing from the shell".
typedef struct _BC250_AMD_IDENTITY_PROCESS {
    ULONG NameFlags;            // BC250_AMD_IDENTITY_NAME_* of dxgkrnl's process name; written at CreateProcess,
                                // before dxgkrnl has the handle, and never again
    volatile LONG Shell;        // the shell's value (BC250_AMD_IDENTITY_SHELL_*) | the NAME_* bits of its name << 8;
                                // written by the escape, read by the query, one interlocked word
    volatile LONG Logged;       // lab flavour: 1 once this process's first answer is in the log
    // What DXGKARG_CREATEPROCESS held, for the lab log (the measurement of the form of dxgkrnl's name); written with
    // NameFlags and never again.
    ULONG NamePointer;          // 1: pProcessName was not NULL
    ULONG NameChars;            // ProcessNameLength as dxgkrnl gave it
    ULONG NameEnd;              // the index of the first zero character in the first NameChars (NameChars: none)
    ULONG NameFirst;            // the first character's value (0: no character)
    ULONG NameFileAt;           // the index of the file name part in that name (0: no '\' or '/')
    char Image[48];             // dxgkrnl's file name in ASCII (at most 47 characters, so that a log line fits
                                // BC250_LOG_TEXT)
    char Head[32];              // the first 31 characters of dxgkrnl's name in ASCII, from index 0
} BC250_AMD_IDENTITY_PROCESS;

// CreateProcess: the record from dxgkrnl's process name (DXGKARG_CREATEPROCESS.pProcessName, ProcessNameLength
// characters, either may be NULL/0).
void AmdIdentityProcessCreate(_Out_ BC250_AMD_IDENTITY_PROCESS* Process, _In_reads_opt_(Chars) const WCHAR* Name,
    UINT Chars);

// QueryAdapterInfo UMDRIVERPRIVATE with BC250_AMD_IDENTITY_BYTES: the reply for the process (NULL: no process
// handle, the answer is OFF). Copies no cache and touches no hardware. PASSIVE_LEVEL.
NTSTATUS AmdIdentityQuery(_In_opt_ BC250_AMD_IDENTITY_PROCESS* Process, _Out_writes_bytes_(Bytes) void* Out, UINT Bytes);

// Bc250Escape, BC250_ESCAPE_SET_PROCESS_POLICY: the shell's value for the process (NULL: no process handle, refused).
void AmdIdentityEscape(_In_opt_ BC250_AMD_IDENTITY_PROCESS* Process, _Inout_ struct _BC250_ESCAPE_PROCESS_POLICY* Data);

// wddm.c: the identity record of a KMD process handle, or NULL when the handle is NULL or is not a process of ours.
BC250_AMD_IDENTITY_PROCESS* WddmProcessIdentity(_In_opt_ HANDLE KmdProcess);
