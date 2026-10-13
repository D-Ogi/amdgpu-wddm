// The installer receipt establishes backup provenance. Only the system worker
// may create the protected files; StartDevice waits for that worker to exit.
#include "bc250kmd.h"
#include "state_kernel.h"
#include "board_memory_store.h"

typedef struct STATE_BOARD_SEED {
    BC250_DEVICE *device;
    AMDGPU_TRANSFER_HEADER receipt;
    NTSTATUS status;
} STATE_BOARD_SEED;

static void SeedWorker(PVOID parameter)
{
    STATE_BOARD_SEED *work=(STATE_BOARD_SEED*)parameter;
    BOARD_STORE_RECORD legacy;BOOLEAN first=FALSE;
    RtlCopyMemory(&legacy,work->receipt.board_record,sizeof(legacy));
    work->status=DriverStateConsumeBoardSeed(&first);
    if(NT_SUCCESS(work->status) && first)
        work->status=BoardMemoryStoreSeed(work->device,
            work->receipt.board_kind==AMDGPU_BOARD_LEGACY?&legacy:NULL,
            work->receipt.board_kind==AMDGPU_BOARD_FRESH);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverStateSeedBoard(BC250_DEVICE *device)
{
    STATE_BOARD_SEED work;OBJECT_ATTRIBUTES attributes;HANDLE thread=NULL;NTSTATUS status;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !device)return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&work,sizeof(work));work.device=device;
    status=DriverStateReceipt(&work.receipt);if(!NT_SUCCESS(status))return status;
    // Unproven legacy provenance does not authorize creating a fresh backup.
    // A complete, protected existing pair may still be read by the store.
    if(work.receipt.board_kind==AMDGPU_BOARD_UNPROVEN)return STATUS_SUCCESS;
    if(work.receipt.board_kind!=AMDGPU_BOARD_FRESH && work.receipt.board_kind!=AMDGPU_BOARD_LEGACY)
        return STATUS_DATA_ERROR;
    InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    status=PsCreateSystemThread(&thread,SYNCHRONIZE,&attributes,NULL,NULL,SeedWorker,&work);
    if(!NT_SUCCESS(status))return status;
    // Non-alertable, no timeout: the stack context cannot outlive this function.
    status=ZwWaitForSingleObject(thread,FALSE,NULL);
    ZwClose(thread);
    return NT_SUCCESS(status)?work.status:status;
}
