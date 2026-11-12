// The registration on \Callback\PowerState that interop.c, dpm.c and cpu.c share (power_notify.h).
#include "bc250kmd.h"

NTSTATUS PowerNotifyRegister(BC250_POWER_NOTIFY* Notify, PCALLBACK_FUNCTION Function, PVOID Context)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    if (Notify->Registration != NULL) return STATUS_SUCCESS;
    RtlInitUnicodeString(&name, L"\\Callback\\PowerState");
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ExCreateCallback(&Notify->Object, &attributes, FALSE, TRUE);
    if (!NT_SUCCESS(status)) {
        Notify->Object = NULL;
        return status;
    }
    Notify->Registration = ExRegisterCallback(Notify->Object, Function, Context);
    if (Notify->Registration == NULL) {
        ObDereferenceObject(Notify->Object);
        Notify->Object = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

void PowerNotifyUnregister(BC250_POWER_NOTIFY* Notify)
{
    if (Notify->Registration != NULL) {
        ExUnregisterCallback(Notify->Registration);
        Notify->Registration = NULL;
    }
    if (Notify->Object != NULL) {
        ObDereferenceObject(Notify->Object);
        Notify->Object = NULL;
    }
}
