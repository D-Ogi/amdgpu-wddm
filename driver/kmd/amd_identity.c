/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// The FSR 4 identity answer: the kernel binding (docs/design/fsr4-identity.md).
//
// AMD's FidelityFX upscaler DLL 4.x reads AMD's family and revision from a 608-byte UMDRIVERPRIVATE query and
// selects its RDNA 2 INT8 provider only for a Navi 2x revision. This file answers that query per process: the
// Navi 22 revision for a process that is ON, the BC-250's own revision for a process that is OFF. Our own caps
// (1472 bytes and more, the BC25 blob) do not change, so our UMDs keep the true identity.
//
// What lives where:
//   amd_identity_policy.c  the anti-cheat list, the precedence and the reply bytes (host-tested,
//                          test/amd_identity_test.c)
//   this file              the process record, the 608-byte answer, the shell's escape and the lab log lines
//   wddm.c                 the hooks: Bc250WddmCreateProcess calls AmdIdentityProcessCreate, QueryAdapterInfo
//                          (UMDRIVERPRIVATE, 608 bytes) AmdIdentityQuery; WddmProcessIdentity maps a handle
//   display.c              Bc250Escape sends BC250_ESCAPE_SET_PROCESS_POLICY to AmdIdentityEscape
//   driver/umd/d3d12/fsr-identity.h  the sender: the D3D12 shell, once per process, at device creation
//
// No registry value is read here: the per-game and global setting FsrUpgrade is the control application's, under
// HKLM\SOFTWARE\amdgpu-wddm\Graphics, and only the shell reads it (app-settings-core.h). The KMD opens no key
// outside its own driver keys.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "amd_identity.h"

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
#define AMD_IDENTITY_LOG_LINES 32
static volatile LONG g_AmdIdentityLogLines;

static BOOLEAN AmdIdentityLogSlot(void)
{
    return InterlockedIncrement(&g_AmdIdentityLogLines) <= AMD_IDENTITY_LOG_LINES;
}
#endif

// The file name part of Name[0..Chars) in ASCII ('?' for any other character), at most Size-1 characters.
static void AmdIdentityNarrow(_Out_writes_(Size) char* Out, ULONG Size, _In_reads_opt_(Chars) const WCHAR* Name,
    UINT Chars)
{
    UINT i, start = 0, end = 0;
    ULONG n = 0;

    Out[0] = 0;
    if (Name == NULL) return;
    while (end < Chars && Name[end] != 0) end++;
    for (i = 0; i < end; i++) if (Name[i] == L'\\' || Name[i] == L'/') start = i + 1;
    for (i = start; i < end && n + 1 < Size; i++) Out[n++] = (Name[i] >= 0x20 && Name[i] < 0x7F) ? (char)Name[i] : '?';
    Out[n] = 0;
}

// The names' flags and the shell's value, as one consistent read of the shell word.
static unsigned int AmdIdentityFacts(_In_ BC250_AMD_IDENTITY_PROCESS* Process, _Out_ unsigned int* Shell)
{
    ULONG word = (ULONG)InterlockedCompareExchange(&Process->Shell, 0, 0);

    *Shell = word & 0xFFu;
    return Process->NameFlags | ((word >> 8) & 0xFFu);
}

void AmdIdentityProcessCreate(_Out_ BC250_AMD_IDENTITY_PROCESS* Process, _In_reads_opt_(Chars) const WCHAR* Name,
    UINT Chars)
{
    RtlZeroMemory(Process, sizeof(*Process));
    if (Name == NULL) Chars = 0;
    Process->NameFlags = Bc250AmdIdentityNameFlags((const unsigned short*)Name, Chars);
    AmdIdentityNarrow(Process->Image, sizeof(Process->Image), Name, Chars);
}

NTSTATUS AmdIdentityQuery(_In_opt_ BC250_AMD_IDENTITY_PROCESS* Process, _Out_writes_bytes_(Bytes) void* Out, UINT Bytes)
{
    unsigned int shell = BC250_AMD_IDENTITY_SHELL_NONE, names = 0, source = 0;
    int on;

    if (Process != NULL) names = AmdIdentityFacts(Process, &shell);
    on = Bc250AmdIdentityDecide(Process != NULL, names, shell, &source);
    if (!Bc250AmdIdentityFill(Out, Bytes, on)) return STATUS_INVALID_PARAMETER;
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    // One line for each process's first answer (every answer without a process), at most AMD_IDENTITY_LOG_LINES.
    if ((Process == NULL || InterlockedExchange(&Process->Logged, 1) == 0) && AmdIdentityLogSlot())
        GuardLog("fsr4-identity: query image %s source %s revision 0x%02X names 0x%X shell %u",
            (Process != NULL && Process->Image[0]) ? Process->Image : "-", Bc250AmdIdentitySourceName(source),
            on ? BC250_AMD_IDENTITY_EREV_NAVI22 : BC250_AMD_IDENTITY_EREV_OWN, names, shell);
#endif
    return STATUS_SUCCESS;
}

void AmdIdentityEscape(_In_opt_ BC250_AMD_IDENTITY_PROCESS* Process, _Inout_ BC250_ESCAPE_PROCESS_POLICY* Data)
{
    unsigned int imageFlags, names, shell, source = 0;
    int on;

    Data->Version = BC250_KMD_VERSION;
    Data->Answer = 0; Data->Source = 0; Data->NameFlags = 0; Data->Reserved1 = 0;
    if (Data->AbiVersion != BC250_PROCESS_POLICY_ABI || Data->FsrIdentity > 1u || Data->Reserved0 != 0) {
        Data->Status = BC250_ESCAPE_STATUS_REFUSED;
        Data->NtStatus = (ULONG)STATUS_INVALID_PARAMETER;
        return;
    }
    if (Process == NULL) {
        // No process handle from dxgkrnl: the query of this process has none either and answers OFF.
        Data->Status = BC250_ESCAPE_STATUS_REFUSED;
        Data->NtStatus = (ULONG)STATUS_NOT_FOUND;
        return;
    }
    // The shell's own image name goes through the same list, so a listed game stays OFF even when dxgkrnl gave no
    // name. Image may lack a terminating zero; the policy reads at most its array.
    imageFlags = Bc250AmdIdentityNameFlags(Data->Image, BC250_PROCESS_POLICY_IMAGE_CHARS);
    InterlockedExchange(&Process->Shell, (LONG)((Data->FsrIdentity ? BC250_AMD_IDENTITY_SHELL_ON : BC250_AMD_IDENTITY_SHELL_OFF) |
        ((imageFlags & 0xFFu) << 8)));
    names = AmdIdentityFacts(Process, &shell);
    on = Bc250AmdIdentityDecide(TRUE, names, shell, &source);
    Data->Answer = (ULONG)on;
    Data->Source = source;
    Data->NameFlags = names;
    Data->Status = BC250_ESCAPE_STATUS_DONE;
    Data->NtStatus = 0;
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (AmdIdentityLogSlot()) {
        char image[32];
        AmdIdentityNarrow(image, sizeof(image), (const WCHAR*)Data->Image, BC250_PROCESS_POLICY_IMAGE_CHARS);
        GuardLog("fsr4-identity: shell image %s FsrUpgrade %lu source %s revision 0x%02X names 0x%X",
            image[0] ? image : "-", Data->FsrIdentity, Bc250AmdIdentitySourceName(source),
            on ? BC250_AMD_IDENTITY_EREV_NAVI22 : BC250_AMD_IDENTITY_EREV_OWN, names);
    }
#endif
}
