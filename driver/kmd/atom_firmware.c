/* WDK 10.0.26100: AuxKlib ACPI table reads and PCI PDO properties at PASSIVE_LEVEL.
 * BDF is used only to match VFCT metadata. PCI reads use the adapter's DXGK callback. */
#include "bc250kmd.h"
#include "atom_firmware.h"
#include <aux_klib.h>

typedef struct _ATOM_FIRMWARE_CONTEXT {
    BC250_DEVICE *Device;
    NTSTATUS Status;
} ATOM_FIRMWARE_CONTEXT;

static int Identity(void *Context, BC250_ATOM_IDENTITY *Id)
{
    ATOM_FIRMWARE_CONTEXT *c = Context;
    UCHAR pci[PCI_COMMON_HDR_LENGTH] = {0};
    ULONG bus = 0, address = 0, returned = 0;
    RtlZeroMemory(Id, sizeof(*Id));
    if (!c->Device->PhysicalDeviceObject || !c->Device->Dxgk.DxgkCbReadDeviceSpace) {
        c->Status = STATUS_INVALID_DEVICE_STATE;
        return -1;
    }
    c->Status = IoGetDeviceProperty(c->Device->PhysicalDeviceObject, DevicePropertyBusNumber,
                                   sizeof(bus), &bus, &returned);
    if (!NT_SUCCESS(c->Status)) return -1;
    if (returned != sizeof(bus)) { c->Status = STATUS_INFO_LENGTH_MISMATCH; return -1; }
    returned = 0;
    c->Status = IoGetDeviceProperty(c->Device->PhysicalDeviceObject, DevicePropertyAddress,
                                   sizeof(address), &address, &returned);
    if (!NT_SUCCESS(c->Status)) return -1;
    if (returned != sizeof(address) || bus > 255 || (address >> 16) > 31 || (address & 0xffffu) > 7) {
        c->Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        return -1;
    }
    returned = 0;
    c->Status = c->Device->Dxgk.DxgkCbReadDeviceSpace(c->Device->Dxgk.DeviceHandle,
        DXGK_WHICHSPACE_CONFIG, pci, 0, sizeof(pci), &returned);
    if (!NT_SUCCESS(c->Status)) return -1;
    if (returned != sizeof(pci)) { c->Status = STATUS_INFO_LENGTH_MISMATCH; return -1; }
    /* Display-class endpoint, not a bridge or a different function in the same package. */
    if (pci[FIELD_OFFSET(PCI_COMMON_CONFIG, BaseClass)] != PCI_CLASS_DISPLAY_CTLR ||
        (pci[FIELD_OFFSET(PCI_COMMON_CONFIG, HeaderType)] & 0x7fu) != PCI_DEVICE_TYPE) {
        c->Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        return -1;
    }
    Id->Bus = bus;
    Id->Device = address >> 16;
    Id->Function = address & 0xffffu;
    RtlCopyMemory(&Id->VendorId, pci + FIELD_OFFSET(PCI_COMMON_CONFIG, VendorID), sizeof(Id->VendorId));
    RtlCopyMemory(&Id->DeviceId, pci + FIELD_OFFSET(PCI_COMMON_CONFIG, DeviceID), sizeof(Id->DeviceId));
    return 0;
}

static int Enumerate(void *Context, void *Buffer, uint32_t Capacity, uint32_t *Returned)
{
    ATOM_FIRMWARE_CONTEXT *c = Context;
    ULONG length = 0;
    c->Status = AuxKlibEnumerateSystemFirmwareTables('ACPI', Buffer, Capacity, &length);
    *Returned = length;
    return NT_SUCCESS(c->Status) || (!Buffer && c->Status == STATUS_BUFFER_TOO_SMALL) ? 0 : -1;
}

static int FetchVfct(void *Context, void *Buffer, uint32_t Capacity, uint32_t *Returned)
{
    ATOM_FIRMWARE_CONTEXT *c = Context;
    ULONG length = 0;
    c->Status = AuxKlibGetSystemFirmwareTable('ACPI', BC250_ATOM_VFCT_ID, Buffer, Capacity, &length);
    *Returned = length;
    return NT_SUCCESS(c->Status) || (!Buffer && c->Status == STATUS_BUFFER_TOO_SMALL) ? 0 : -1;
}

static void *Allocate(void *Context, size_t Bytes)
{
    UNREFERENCED_PARAMETER(Context);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Bytes, 'AfAB');
}

static void Free(void *Context, void *Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    ExFreePoolWithTag(Buffer, 'AfAB');
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AtomFirmwareCapture(BC250_DEVICE *Device, BC250_ATOM_FIRMWARE *Firmware, BC250_ATOM_STATUS *Reason)
{
    ATOM_FIRMWARE_CONTEXT context = {Device, STATUS_SUCCESS};
    BC250_ATOM_FIRMWARE_IO io = {&context, Identity, Enumerate, FetchVfct, Allocate, Free};
    BC250_ATOM_STATUS result;
    if (Reason) *Reason = BC250_ATOM_ARGUMENT;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (!Device || !Firmware || !Reason || Firmware->Storage || Firmware->Image.Bytes) return STATUS_INVALID_PARAMETER;
    context.Status = AuxKlibInitialize();
    if (!NT_SUCCESS(context.Status)) { *Reason = BC250_ATOM_IO; return context.Status; }
    result = Bc250AtomFirmwareCapture(&io, Firmware);
    *Reason = result;
    if (result == BC250_ATOM_OK) return STATUS_SUCCESS;
    if (result == BC250_ATOM_MEMORY) return STATUS_INSUFFICIENT_RESOURCES;
    if (result == BC250_ATOM_IO && !NT_SUCCESS(context.Status)) return context.Status;
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

_IRQL_requires_(PASSIVE_LEVEL)
void AtomFirmwareRelease(BC250_ATOM_FIRMWARE *Firmware)
{
    BC250_ATOM_FIRMWARE_IO io = {0};
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    io.Free = Free;
    Bc250AtomFirmwareRelease(&io, Firmware);
}
