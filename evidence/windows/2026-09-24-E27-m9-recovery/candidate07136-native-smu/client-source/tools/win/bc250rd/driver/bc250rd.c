// bc250rd.sys - read-only register reader for experiment E02.
//
// A software-only WDM driver: it does not bind to the GPU, so the Microsoft Basic Display driver keeps the
// device and the screen. It finds 1002:13FE in PCI configuration space, maps BAR5 with a read-only mapping
// and returns 32-bit reads, but only for offsets in allowlist.h: a read of the wrong BAR5 address can hang
// this SoC (docs/facts.md M16).
//
// SMU ownership is being moved to the KMD integration. This instrument contains no
// mailbox write path. Old IOCTL_BC250RD_SMU_MSG callers receive NOT_SUPPORTED;
// there is no automatic direct-access fallback while KMD is unavailable.
// Deploy only with the matching KMD control clients after handover validation.
//
// Temperature: the thermal sensor is an SMN register. It is read through the root complex's SMN index/data
// pair (configuration space 0x60/0x64 of 00:00.0), exactly as Linux' k10temp does on this unit. That is one
// write, to the host bridge's index register, restored afterwards; SMN addresses are allow-listed too.
//
// Not a product driver: HalGetBusDataByOffset is a legacy way to read configuration space, acceptable for
// a measurement tool that must stay out of the device stack.

#include <ntddk.h>
#include <wdmsec.h>
#include "../bc250rd_ioctl.h"
#include "allowlist.h"

#pragma warning(disable : 4996)   // HalGetBusDataByOffset is deprecated, see above

static PUCHAR g_Bar5;             // read-only mapping, NULL until attached
static BC250RD_INFO g_Info;
static FAST_MUTEX g_Lock;

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD Bc250Unload;
static DRIVER_DISPATCH Bc250CreateClose;
static DRIVER_DISPATCH Bc250DeviceControl;

static BOOLEAN IsAllowed(ULONG Offset)
{
    LONG lo = 0, hi = BC250RD_ALLOW_COUNT - 1;
    while (lo <= hi) {
        LONG mid = (lo + hi) / 2;
        if (g_Allow[mid] == Offset) return TRUE;
        if (g_Allow[mid] < Offset) lo = mid + 1; else hi = mid - 1;
    }
    return FALSE;
}

static const ULONG g_SmnAllow[] = { BC250RD_SMN_THM_TCON_CUR_TMP };

static NTSTATUS SmnRead(ULONG Address, PULONG Value)
{
    PCI_SLOT_NUMBER slot = { 0 };   // 00:00.0
    USHORT ids[2] = { 0, 0 };
    ULONG saved = 0;
    BOOLEAN allowed = FALSE;

    for (ULONG i = 0; i < RTL_NUMBER_OF(g_SmnAllow); i++) allowed = allowed || g_SmnAllow[i] == Address;
    if (!allowed) return STATUS_ACCESS_DENIED;

    if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, ids, 0, sizeof(ids)) != sizeof(ids) ||
        ids[0] != BC250RD_HOST_BRIDGE_VENDOR || ids[1] != BC250RD_HOST_BRIDGE_DEVICE)
        return STATUS_NO_SUCH_DEVICE;

    if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &saved, 0x60, 4) != 4) return STATUS_IO_DEVICE_ERROR;
    if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &Address, 0x60, 4) != 4) return STATUS_IO_DEVICE_ERROR;
    ULONG got = HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, Value, 0x64, 4);
    HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &saved, 0x60, 4);
    return got == 4 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static NTSTATUS Attach(void)
{
    if (g_Bar5) return STATUS_SUCCESS;

    for (ULONG bus = 0; bus < 32; bus++) {
        for (ULONG dev = 0; dev < 32; dev++) {
            for (ULONG fn = 0; fn < 8; fn++) {
                PCI_SLOT_NUMBER slot = { 0 };
                PCI_COMMON_CONFIG cfg;
                slot.u.bits.DeviceNumber = dev;
                slot.u.bits.FunctionNumber = fn;
                ULONG got = HalGetBusDataByOffset(PCIConfiguration, bus, slot.u.AsULONG, &cfg, 0, PCI_COMMON_HDR_LENGTH);
                if (got < PCI_COMMON_HDR_LENGTH || cfg.VendorID != BC250RD_VENDOR_ID || cfg.DeviceID != BC250RD_DEVICE_ID)
                    continue;

                ULONG bar5 = cfg.u.type0.BaseAddresses[5];
                if ((cfg.HeaderType & 0x7F) != 0 || (bar5 & 1) != 0 || (bar5 & ~0xFul) == 0)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;   // not a memory BAR or not assigned
                if ((cfg.Command & PCI_ENABLE_MEMORY_SPACE) == 0)
                    return STATUS_DEVICE_NOT_READY;             // decoding off: reads would not reach the GPU

                PHYSICAL_ADDRESS pa;
                pa.QuadPart = bar5 & ~0xFul;
                PVOID va = MmMapIoSpaceEx(pa, BC250RD_BAR5_SIZE, PAGE_READONLY | PAGE_NOCACHE);
                if (!va) return STATUS_INSUFFICIENT_RESOURCES;
                g_Info.Bus = bus; g_Info.Device = dev; g_Info.Function = fn;
                g_Info.VendorId = cfg.VendorID; g_Info.DeviceId = cfg.DeviceID;
                g_Info.Command = cfg.Command; g_Info.Status = cfg.Status; g_Info.RevisionId = cfg.RevisionID;
                for (int i = 0; i < 6; i++) g_Info.Bars[i] = cfg.u.type0.BaseAddresses[i];
                g_Info.Bar5Physical = (unsigned long long)pa.QuadPart;
                g_Info.Bar5Size = BC250RD_BAR5_SIZE;
                g_Info.AllowCount = BC250RD_ALLOW_COUNT;
                g_Bar5 = (PUCHAR)va;
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS Bc250DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PULONG buf = (PULONG)Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR written = 0;

    ExAcquireFastMutex(&g_Lock);
    switch (sp->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_BC250RD_ATTACH:
        if (outLen < sizeof(BC250RD_INFO)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = Attach();
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(buf, &g_Info, sizeof(g_Info));
            written = sizeof(g_Info);
        }
        break;

    case IOCTL_BC250RD_READ: {
        ULONG count = inLen / sizeof(ULONG);
        if (!g_Bar5) { status = STATUS_DEVICE_NOT_READY; break; }
        if (count == 0 || count > BC250RD_MAX_READS || inLen % sizeof(ULONG) || outLen < inLen) {
            status = STATUS_INVALID_PARAMETER; break;
        }
        status = STATUS_SUCCESS;
        for (ULONG i = 0; i < count; i++) {
            if (!IsAllowed(buf[i])) { status = STATUS_ACCESS_DENIED; break; }
        }
        if (!NT_SUCCESS(status)) break;
        for (ULONG i = 0; i < count; i++)   // METHOD_BUFFERED: input and output share the buffer
            buf[i] = READ_REGISTER_ULONG((PULONG)(g_Bar5 + buf[i]));
        written = (ULONG_PTR)count * sizeof(ULONG);
        break;
    }

    case IOCTL_BC250RD_SMU_MSG:
        if (inLen != sizeof(BC250RD_SMU_MSG) || outLen < sizeof(BC250RD_SMU_MSG)) { status = STATUS_INVALID_PARAMETER; break; }
        // Permanently removed from this binary, including during PnP gaps.
        status = STATUS_NOT_SUPPORTED;
        break;

    case IOCTL_BC250RD_SMN_READ: {
        ULONG value = 0;
        if (inLen != sizeof(ULONG) || outLen < sizeof(ULONG)) { status = STATUS_INVALID_PARAMETER; break; }
        status = SmnRead(buf[0], &value);
        if (NT_SUCCESS(status)) { buf[0] = value; written = sizeof(ULONG); }
        break;
    }
    }
    ExReleaseFastMutex(&g_Lock);

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = written;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS Bc250CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID Bc250Unload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos = RTL_CONSTANT_STRING(BC250RD_DEVICE_DOS);
    IoDeleteSymbolicLink(&dos);
    if (g_Bar5) MmUnmapIoSpace(g_Bar5, BC250RD_BAR5_SIZE);
    if (DriverObject->DeviceObject) IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    UNICODE_STRING nt = RTL_CONSTANT_STRING(BC250RD_DEVICE_NT);
    UNICODE_STRING dos = RTL_CONSTANT_STRING(BC250RD_DEVICE_DOS);
    // {6f1e1a36-7c0b-4f0e-9d53-bc250d0e0201}: device class for the security descriptor lookup, ours alone
    static const GUID classGuid = { 0x6f1e1a36, 0x7c0b, 0x4f0e, { 0x9d, 0x53, 0xbc, 0x25, 0x0d, 0x0e, 0x02, 0x01 } };
    PDEVICE_OBJECT device = NULL;

    ExInitializeFastMutex(&g_Lock);
    NTSTATUS status = IoCreateDeviceSecure(DriverObject, 0, &nt, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
                                           &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, &classGuid, &device);
    if (!NT_SUCCESS(status)) return status;
    status = IoCreateSymbolicLink(&dos, &nt);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(device); return status; }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = Bc250CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = Bc250CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Bc250DeviceControl;
    DriverObject->DriverUnload = Bc250Unload;
    return STATUS_SUCCESS;
}
