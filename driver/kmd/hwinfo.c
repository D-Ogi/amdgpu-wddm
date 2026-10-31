// BD-245: HardwareInformation registry values for the display adapter.
// Written once per start, after the board identity and VRAM are known.
#include "bc250kmd.h"
#include <ntstrsafe.h>

static NTSTATUS WriteString(HANDLE Key, PCWSTR Name, PCWSTR Value)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    return ZwSetValueKey(Key, &name, 0, REG_BINARY, (PVOID)Value,
                         (ULONG)((wcslen(Value) + 1) * sizeof(WCHAR)));
}

// The CHIP_CYAN_SKILLFISH rows of amdgpu's pciidlist (drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c). Other devices
// get the hexadecimal fallback: a codename table typed from memory gave wrong names.
static PCWSTR ChipName(USHORT DeviceId)
{
    switch (DeviceId) {
    case 0x13DB: case 0x13F9: case 0x13FA: case 0x13FB: case 0x13FC: case 0x13FE: case 0x143F:
        return L"Cyan Skillfish";
    default:
        return NULL;
    }
}

void HardwareInfoWrite(BC250_DEVICE* Device)
{
    HANDLE key;
    NTSTATUS status;
    UNICODE_STRING valueName;
    ULONG memorySize;
    ULONGLONG qwMemorySize;
    WCHAR adapter[64];
    PCWSTR chip;
    UCHAR pci[4] = {0};
    ULONG pciBytes = 0;
    USHORT deviceId = 0;

    if (!Device->PhysicalDeviceObject) return;
    status = IoOpenDeviceRegistryKey(Device->PhysicalDeviceObject,
        PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key);
    if (!NT_SUCCESS(status)) {
        GuardLog("hwinfo: open driver key 0x%08X", (ULONG)status);
        return;
    }

    if (Device->Dxgk.DxgkCbReadDeviceSpace) {
        status = Device->Dxgk.DxgkCbReadDeviceSpace(Device->Dxgk.DeviceHandle,
            DXGK_WHICHSPACE_CONFIG, pci, 2, 2, &pciBytes);
        if (NT_SUCCESS(status) && pciBytes == 2)
            deviceId = (USHORT)pci[0] | ((USHORT)pci[1] << 8);
    }

    chip = ChipName(deviceId);
    if (chip)
        RtlStringCchPrintfW(adapter, RTL_NUMBER_OF(adapter), L"AMD %s", chip);
    else if (deviceId)
        RtlStringCchPrintfW(adapter, RTL_NUMBER_OF(adapter), L"AMD Radeon (%04X)", (ULONG)deviceId);
    else
        RtlStringCchPrintfW(adapter, RTL_NUMBER_OF(adapter), L"AMD Radeon");

    (void)WriteString(key, L"HardwareInformation.AdapterString", adapter);
    (void)WriteString(key, L"HardwareInformation.ChipType", chip ? chip : L"AMD ASIC");
    (void)WriteString(key, L"HardwareInformation.DacType", L"Internal");
    (void)WriteString(key, L"HardwareInformation.BiosString", L"AMDGPU-WDDM");

    qwMemorySize = Device->VramLength;
    memorySize = qwMemorySize > MAXULONG ? MAXULONG : (ULONG)qwMemorySize;
    RtlInitUnicodeString(&valueName, L"HardwareInformation.MemorySize");
    (void)ZwSetValueKey(key, &valueName, 0, REG_DWORD, &memorySize, sizeof(memorySize));
    RtlInitUnicodeString(&valueName, L"HardwareInformation.qwMemorySize");
    (void)ZwSetValueKey(key, &valueName, 0, REG_QWORD, &qwMemorySize, sizeof(qwMemorySize));

    ZwClose(key);
    GuardLog("hwinfo: %S memory %llu", adapter, qwMemorySize);
}
