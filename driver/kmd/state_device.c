#include <ntifs.h>
#include <initguid.h>
#include <devpkey.h>
#include "state_kernel.h"
#include "../state/identity.h"
#include "../state/transfer.h"
typedef struct STATE_ACTIVATE {
    unsigned char hash[32];
    NTSTATUS status;
} STATE_ACTIVATE;
static void ActivateWorker(PVOID context)
{
    STATE_ACTIVATE *work=(STATE_ACTIVATE*)context;
    work->status=DriverStateActivate(work->hash);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS ResolveDeviceIdentity(PDEVICE_OBJECT device,unsigned char hash[32])
{
    WCHAR instance[513];ULONG needed=0,i;DEVPROPTYPE type=0;NTSTATUS status;
    AMDGPU_TRANSFER_HEADER receipt;unsigned int any=0;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !device)return STATUS_INVALID_DEVICE_STATE;
    status=DriverStateReceipt(&receipt);if(!NT_SUCCESS(status))return status;
    for(i=0;i<32;i++)any|=receipt.device_hash[i];
    RtlZeroMemory(hash,32);
    if(!any) {
        if(receipt.origin!=AMDGPU_TRANSFER_FRESH || receipt.board_kind!=AMDGPU_BOARD_UNPROVEN)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
    } else {
        status=IoGetDevicePropertyData(device,&DEVPKEY_Device_InstanceId,LOCALE_NEUTRAL,0,sizeof(instance),
                                   instance,&needed,&type);
        if(!NT_SUCCESS(status))return status;
        if(type!=DEVPROP_TYPE_STRING || needed<4 || needed>sizeof(instance) || needed%2 || instance[needed/2-1])
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        if(AmdgpuStateDeviceIdentity(instance,needed/2-1,hash)!=AMDGPU_STATE_OK ||
           RtlCompareMemory(hash,receipt.device_hash,32)!=32)return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}
NTSTATUS DriverStateValidateDevice(PDEVICE_OBJECT device)
{
    unsigned char hash[32];
    // AddDevice validates ownership without publishing or flushing state.
    return ResolveDeviceIdentity(device,hash);
}
NTSTATUS DriverStateActivateDevice(PDEVICE_OBJECT device)
{
    STATE_ACTIVATE work;NTSTATUS status;OBJECT_ATTRIBUTES attributes;HANDLE thread=NULL;
    RtlZeroMemory(&work,sizeof(work));
    // Recheck the PDO at StartDevice. Only this SYSTEM worker may publish the
    // prepared generation, before diagnostics, start admission or hardware I/O.
    status=ResolveDeviceIdentity(device,work.hash);if(!NT_SUCCESS(status))return status;
    InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    status=PsCreateSystemThread(&thread,SYNCHRONIZE,&attributes,NULL,NULL,ActivateWorker,&work);
    if(!NT_SUCCESS(status))return status;
    status=ZwWaitForSingleObject(thread,FALSE,NULL);ZwClose(thread);
    return NT_SUCCESS(status)?work.status:status;
}
