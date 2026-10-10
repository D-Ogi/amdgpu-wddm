// UMA control capability. No CMOS access is admitted without a qualified transport.
// See docs/design/uma-reservation.md for the unresolved HAL/firmware contract.
#include "bc250kmd.h"

void UmaRequest(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_UMA* Data,
                _In_ BOOLEAN Admin, _In_ ULONG EscapeFlags)
{
    BC250_ESCAPE_UMA request = *Data;
    D3DDDI_ESCAPEFLAGS wanted = {0};
    ULONG i;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    C_ASSERT(sizeof(BC250_ESCAPE_UMA) == 128);

    RtlZeroMemory(Data, sizeof(*Data));
    Data->Magic = BC250_ESCAPE_MAGIC;
    Data->Command = BC250_ESCAPE_RUN_UMA;
    Data->AbiVersion = BC250_UMA_ABI;
    Data->Op = request.Op;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->Reason = BC250_UMA_REASON_NO_TRANSPORT;
    if (request.Magic != BC250_ESCAPE_MAGIC || request.Command != BC250_ESCAPE_RUN_UMA ||
        request.AbiVersion != BC250_UMA_ABI || request.Op > BC250_UMA_OP_RESTORE ||
        request.Status != BC250_ESCAPE_STATUS_UNKNOWN_COMMAND || request.NtStatus || request.Flags ||
        request.Reason || request.ActiveBytes || request.PreviousMiB || request.ResultCode) goto done;
    for (i = 0; i < RTL_NUMBER_OF(request.Reserved); ++i)
        if (request.Reserved[i]) goto done;
    if (request.Op == BC250_UMA_OP_READ) {
        wanted.NoAdapterSynchronization = 1;
        if (EscapeFlags != wanted.Value || request.RequestedMiB) goto done;
        for (i = 0; i < sizeof(request.ObservedBlock); ++i)
            if (request.ObservedBlock[i]) goto done;
        Data->ActiveBytes = (ULONGLONG)InterlockedCompareExchange64(&Device->UmaActiveBytes, 0, 0);
        if (Data->ActiveBytes) Data->Flags = BC250_UMA_ACTIVE_VALID;
        Data->Status = BC250_ESCAPE_STATUS_DONE;
        status = STATUS_SUCCESS;
    } else {
        wanted.HardwareAccess = 1;
        if (EscapeFlags != wanted.Value) goto done;
        if (request.Op == BC250_UMA_OP_SET && request.RequestedMiB != 8192 &&
            request.RequestedMiB != 12288) goto done;
        if (request.Op == BC250_UMA_OP_RESTORE && request.RequestedMiB) goto done;
        if (!Admin) {
            Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
            status = STATUS_ACCESS_DENIED;
        } else {
            // No registry switch bypasses this boundary. Do not substitute raw
            // ports, guessed HAL parameters or a private lock for OS ownership.
            status = STATUS_NOT_SUPPORTED;
        }
    }
done:
    Data->NtStatus = (ULONG)status;
}
