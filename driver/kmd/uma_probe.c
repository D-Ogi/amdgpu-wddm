// Read-only HAL CMOS mapping diagnostic. It does not qualify a write transport.
#include "bc250kmd.h"

static ULONG UmaProbeRead(ULONG Bus, ULONG Slot, PVOID Buffer, ULONG Offset, ULONG Bytes)
{
    ULONG count;
    /* WDK 26100 ntddk.h explicitly exempts Cmos from the obsolete-bus warning.
       The slot/offset interpretation for extended CMOS is what this probe tests. */
#pragma warning(push)
#pragma warning(disable:4996)
    count = HalGetBusDataByOffset(Cmos, Bus, Slot, Buffer, Offset, Bytes);
#pragma warning(pop)
    if (count < Bytes) RtlZeroMemory((PUCHAR)Buffer + count, Bytes - count);
    if (count > Bytes) RtlZeroMemory(Buffer, Bytes);
    return count;
}

void UmaProbeRequest(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_UMA_PROBE* Data,
                     _In_ BOOLEAN Admin, _In_ ULONG EscapeFlags)
{
    BC250_ESCAPE_UMA_PROBE request = *Data, canonical;
    D3DDDI_ESCAPEFLAGS wanted = {0};
    static const ULONG rtcSlots[4] = {0, 2, 4, 0x0d};
    ULONG i;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    C_ASSERT(sizeof(BC250_ESCAPE_UMA_PROBE) == 128);
    RtlZeroMemory(&canonical, sizeof(canonical));
    canonical.Magic = BC250_ESCAPE_MAGIC;
    canonical.Command = BC250_ESCAPE_RUN_UMA;
    canonical.Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    canonical.AbiVersion = BC250_UMA_ABI;
    canonical.Op = BC250_UMA_OP_PROBE;
    RtlZeroMemory(Data, sizeof(*Data));
    Data->Magic = canonical.Magic;
    Data->Command = canonical.Command;
    Data->AbiVersion = canonical.AbiVersion;
    Data->Op = canonical.Op;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->Reason = BC250_UMA_REASON_READ;
    if (RtlCompareMemory(&request, &canonical, sizeof(request)) != sizeof(request)) goto done;
    if (!Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        status = STATUS_ACCESS_DENIED;
        goto done;
    }
    wanted.HardwareAccess = 1;
    if (EscapeFlags != wanted.Value) goto done;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        InterlockedCompareExchange64(&Device->UmaActiveBytes, 0, 0) == 0 ||
        InterlockedCompareExchange(&Device->RetainedPowerPhase, 0, 0) != 0) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto done;
    }
    Data->SlotCount = UmaProbeRead(1, 0x90, Data->SlotBlock, 0, sizeof(Data->SlotBlock));
    Data->OffsetCount = UmaProbeRead(1, 0, Data->OffsetBlock, 0x90, sizeof(Data->OffsetBlock));
    for (i = 0; i < 4; ++i)
        Data->RtcCount[i] = UmaProbeRead(0, rtcSlots[i], &Data->RtcValue[i], 0, 1);
    if (Data->SlotCount > sizeof(Data->SlotBlock) || Data->OffsetCount > sizeof(Data->OffsetBlock)) {
        status = STATUS_DATA_ERROR;
        goto done;
    }
    for (i = 0; i < 4; ++i) if (Data->RtcCount[i] > 1) {
        status = STATUS_DATA_ERROR;
        goto done;
    }
    /* DONE means all six read calls returned, including zero/short counts.
       Raw blocks/counts remain unqualified; no signature or mapping is inferred. */
    Data->Status = BC250_ESCAPE_STATUS_DONE;
    status = STATUS_SUCCESS;
done:
    Data->NtStatus = (ULONG)status;
}
