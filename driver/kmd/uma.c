// Board memory capability and the strict public escape boundary.
#include "bc250kmd.h"

static void AblQuery(BC250_DEVICE* Device, BC250_ESCAPE_BOARD_MEMORY* Data)
{
    Data->ProviderId = BC250_BOARD_MEMORY_PROVIDER_BC250_ABL;
    Data->Flags = BC250_BOARD_MEMORY_SUPPORTED;
    Data->AllowedMiB[0] = 8192;
    Data->AllowedMiB[1] = 12288;
    Data->NeedsRestart = 1;
    Data->ActiveBytes = (ULONGLONG)InterlockedCompareExchange64(&Device->UmaActiveBytes, 0, 0);
    if (Data->ActiveBytes) Data->Flags |= BC250_BOARD_MEMORY_ACTIVE_VALID;
    Data->Reason = BC250_BOARD_MEMORY_REASON_NO_TRANSPORT;
    BoardMemoryWriteQuery(Device, Data);
}
const BC250_BOARD_MEMORY_PROVIDER* BoardMemoryProvider(BC250_DEVICE* Device)
{
    static const BC250_BOARD_MEMORY_PROVIDER abl = {
        AblQuery, AblBoardMemorySet, AblBoardMemoryRestore, AblBoardMemoryProbeRequest
    };
    return InterlockedCompareExchange(&Device->BoardMemoryProviderId, 0, 0) ==
        BC250_BOARD_MEMORY_PROVIDER_BC250_ABL ? &abl : NULL;
}

void BoardMemoryRequest(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_BOARD_MEMORY* Data,
                _In_ BOOLEAN Admin, _In_ ULONG EscapeFlags)
{
    BC250_ESCAPE_BOARD_MEMORY request = *Data;
    D3DDDI_ESCAPEFLAGS wanted = {0};
    ULONG i;
    const BC250_BOARD_MEMORY_PROVIDER* provider = BoardMemoryProvider(Device);
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    C_ASSERT(sizeof(BC250_ESCAPE_BOARD_MEMORY) == 128);

    RtlZeroMemory(Data, sizeof(*Data));
    Data->Magic = BC250_ESCAPE_MAGIC;
    Data->Command = BC250_ESCAPE_RUN_BOARD_MEMORY;
    Data->AbiVersion = BC250_BOARD_MEMORY_ABI;
    Data->Op = request.Op;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->Reason = provider ? BC250_BOARD_MEMORY_REASON_NO_TRANSPORT : Device->BoardMemoryReason;
    if (request.Magic != BC250_ESCAPE_MAGIC || request.Command != BC250_ESCAPE_RUN_BOARD_MEMORY ||
        request.AbiVersion != BC250_BOARD_MEMORY_ABI || request.Op > BC250_BOARD_MEMORY_OP_RESTORE ||
        request.Status != BC250_ESCAPE_STATUS_UNKNOWN_COMMAND || request.NtStatus || request.Flags ||
        request.Reason || request.ActiveBytes || request.PreviousMiB || request.ResultCode || request.ProviderId ||
        request.AllowedMiB[0] || request.AllowedMiB[1] || request.NeedsRestart) goto done;
    for (i = 0; i < RTL_NUMBER_OF(request.Reserved); ++i)
        if (request.Reserved[i]) goto done;
    if (request.Op == BC250_BOARD_MEMORY_OP_READ) {
        wanted.NoAdapterSynchronization = 1;
        if (EscapeFlags != wanted.Value || request.RequestedMiB) goto done;
        for (i = 0; i < sizeof(request.ObservedBlock); ++i)
            if (request.ObservedBlock[i]) goto done;
        if (provider) provider->Query(Device, Data);
        Data->Status = BC250_ESCAPE_STATUS_DONE;
        status = STATUS_SUCCESS;
    } else {
        wanted.HardwareAccess = 1;
        if (EscapeFlags != wanted.Value) goto done;
        if (request.Op == BC250_BOARD_MEMORY_OP_SET && request.RequestedMiB != 8192 &&
            request.RequestedMiB != 12288) goto done;
        if (request.Op == BC250_BOARD_MEMORY_OP_RESTORE && request.RequestedMiB) goto done;
        if (!Admin) {
            Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
            status = STATUS_ACCESS_DENIED;
        } else {
            // No registry switch bypasses this boundary. Do not substitute raw
            // ports, guessed HAL parameters or a private lock for OS ownership.
            status = provider ? (request.Op == BC250_BOARD_MEMORY_OP_SET ?
                provider->Set(Device, &request) : provider->Restore(Device, &request)) : STATUS_NOT_SUPPORTED;
            if (provider) provider->Query(Device, Data);
            if (NT_SUCCESS(status)) Data->Status = BC250_ESCAPE_STATUS_DONE;
        }
    }
done:
    Data->NtStatus = (ULONG)status;
}
