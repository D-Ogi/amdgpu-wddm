#include "bc250kmd.h"
#include "board_identity.h"
#include <aux_klib.h>

void BoardMemoryIdentityClear(BC250_DEVICE* Device)
{
    InterlockedExchange(&Device->BoardMemoryProviderId, BC250_BOARD_MEMORY_PROVIDER_NONE);
    Device->BoardMemoryReason = BC250_BOARD_MEMORY_REASON_BOARD;
    Device->BoardMemoryBiosId = 0;
    RtlZeroMemory(Device->BoardMemoryMachineId, sizeof(Device->BoardMemoryMachineId));
}
void BoardMemoryIdentityCapture(BC250_DEVICE* Device)
{
    UCHAR pci[64] = {0};
    UCHAR* data = NULL;
    ULONG bytes = 0, capacity = 0;
    unsigned reason = BC250_BOARD_MEMORY_REASON_BOARD, provider;
    NTSTATUS status;
    BoardMemoryIdentityClear(Device);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !Device->Dxgk.DxgkCbReadDeviceSpace) return;
    status = Device->Dxgk.DxgkCbReadDeviceSpace(Device->Dxgk.DeviceHandle,
        DXGK_WHICHSPACE_CONFIG, pci, 0, sizeof(pci), &bytes);
    if (!NT_SUCCESS(status) || bytes != sizeof(pci)) return;
    /* Avoid firmware calls for a foreign PCI function. */
    (void)BoardIdentitySelect(pci, bytes, NULL, 0, &reason);
    if (reason == BC250_BOARD_MEMORY_REASON_BOARD) return;
    Device->BoardMemoryReason = BC250_BOARD_MEMORY_REASON_FIRMWARE;
    if (!NT_SUCCESS(AuxKlibInitialize())) return;
    status = AuxKlibGetSystemFirmwareTable('RSMB', 0, NULL, 0, &capacity);
    if ((!NT_SUCCESS(status) && status != STATUS_BUFFER_TOO_SMALL) || capacity < 8 || capacity > 1024 * 1024)
        return;
    data = ExAllocatePool2(POOL_FLAG_NON_PAGED, capacity, 'ImAB');
    if (!data) return;
    bytes = 0;
    status = AuxKlibGetSystemFirmwareTable('RSMB', 0, data, capacity, &bytes);
    if (NT_SUCCESS(status) && bytes == capacity) {
        provider = BoardIdentitySelect(pci, sizeof(pci), data, bytes, &reason);
        if (provider == BC250_BOARD_MEMORY_PROVIDER_BC250_ABL) {
            unsigned bios = 0;
            if (BoardIdentityQualification(data, bytes, &bios, Device->BoardMemoryMachineId))
                Device->BoardMemoryBiosId = bios;
        }
        Device->BoardMemoryReason = reason;
        InterlockedExchange(&Device->BoardMemoryProviderId, (LONG)provider);
    }
    ExFreePoolWithTag(data, 'ImAB');
}
