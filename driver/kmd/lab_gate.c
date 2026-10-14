/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// BD-167. The runtime gate for the lab-only raw escapes (see lab_gate.h). Compiled to nothing in the release
// flavour: the whole translation unit is behind AMDGPU_WDDM_LAB_FLAVOUR, which only driver\kmd\build.ps1 -Lab sets.
#include "bc250kmd.h"
#include "lab_gate.h"

#ifdef AMDGPU_WDDM_LAB_FLAVOUR

// Windows reports whether the running kernel enforces test signing through SystemCodeIntegrityInformation. The
// class, the structure and the bit are declared in the user-mode header winternl.h (SDK 10.0.26100.0):
//   um\winternl.h:401  SystemCodeIntegrityInformation = 103   (SYSTEM_INFORMATION_CLASS)
//   um\winternl.h:387  typedef struct _SYSTEM_CODEINTEGRITY_INFORMATION { ULONG Length; ULONG CodeIntegrityOptions; }
//   um\winternl.h:372  #define CODEINTEGRITY_OPTION_TESTSIGN 0x02
// winternl.h is user mode, so it cannot be included here; the three declarations are reproduced with that
// provenance. ZwQuerySystemInformation is exported by ntoskrnl.lib but is not declared in the WDK km headers,
// so it is declared locally as well (same signature as the documented export).
#define BC250_SYSTEM_CODE_INTEGRITY_INFORMATION 103u
#define BC250_CODEINTEGRITY_OPTION_TESTSIGN     0x02u

typedef struct _BC250_SYSTEM_CODEINTEGRITY_INFORMATION {
    ULONG Length;
    ULONG CodeIntegrityOptions;
} BC250_SYSTEM_CODEINTEGRITY_INFORMATION;

NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

// Test signing cannot change without a reboot, so the query runs once. 0 = not yet read, 1 = on, 2 = off.
static LONG g_TestSignState;

static BOOLEAN LabTestSigningOn(void)
{
    BC250_SYSTEM_CODEINTEGRITY_INFORMATION info;
    ULONG returned = 0;
    NTSTATUS status;
    LONG cached = InterlockedCompareExchange(&g_TestSignState, 0, 0);

    if (cached != 0) return cached == 1;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;   // the query needs PASSIVE_LEVEL; refuse rather than guess

    RtlZeroMemory(&info, sizeof(info));
    info.Length = sizeof(info);
    status = ZwQuerySystemInformation(BC250_SYSTEM_CODE_INTEGRITY_INFORMATION, &info, sizeof(info), &returned);
    if (!NT_SUCCESS(status) || returned < sizeof(info)) {
        GuardLog("labgate: code-integrity query 0x%08X returned %lu; test signing treated as off", status, returned);
        return FALSE;   // not cached: a transient failure must not latch "off" for the driver's life
    }
    cached = (info.CodeIntegrityOptions & BC250_CODEINTEGRITY_OPTION_TESTSIGN) ? 1 : 2;
    InterlockedExchange(&g_TestSignState, cached);
    GuardLog("labgate: CodeIntegrityOptions 0x%08X, test signing %s", info.CodeIntegrityOptions,
             cached == 1 ? "on" : "off");
    return cached == 1;
}

// PASSIVE_LEVEL. The raw diagnostic escapes answer only when all three hold: this is a lab build (the whole file
// is behind AMDGPU_WDDM_LAB_FLAVOUR), the operator has set Parameters\LabDiagnostics = 1 (the installer never does),
// and the kernel runs with test signing on. A production-signed driver therefore never reaches the raw path.
BOOLEAN LabDiagnosticsAllowed(void)
{
    if (GuardReadSetting(L"LabDiagnostics", 0) != 1) return FALSE;
    return LabTestSigningOn();
}

// Once per family and driver load, so that a tool that calls GET_MEMORY in a loop does not fill the log ring.
static LONG g_LabEscapesSeen;

void LabEscapeAdmitted(BC250_LAB_ESCAPE Family, const char* Marker)
{
    LONG bit;

    if ((ULONG)Family >= (ULONG)LabEscapeCount) return;
    bit = 1L << Family;
    if (InterlockedOr(&g_LabEscapesSeen, bit) & bit) return;
    GuardLog("labgate: first use since load: %s", Marker);
}

#endif /* AMDGPU_WDDM_LAB_FLAVOUR */
