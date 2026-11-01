// BD-245: HardwareInformation registry values for the display adapter.
// Written once per start, after the board identity and VRAM are known.
#include "bc250kmd.h"
#include <ntstrsafe.h>
#include "chip_table_gen.h"

#define ATOM_STRING_COUNT_OFFSET 0x2fu
#define ATOM_STRING_START_OFFSET 0x6eu

static NTSTATUS WriteString(HANDLE Key, PCWSTR Name, PCWSTR Value)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    return ZwSetValueKey(Key, &name, 0, REG_BINARY, (PVOID)Value,
                         (ULONG)((wcslen(Value) + 1) * sizeof(WCHAR)));
}

static PCWSTR VbiosPartNumber(const UCHAR *Rom, ULONG Length,
                              WCHAR *Buffer, SIZE_T Count)
{
    USHORT strOff;
    SIZE_T i;
    if (Length < ATOM_STRING_START_OFFSET + 2) return NULL;
    if (Rom[ATOM_STRING_COUNT_OFFSET] == 0) return NULL;
    strOff = (USHORT)Rom[ATOM_STRING_START_OFFSET] |
             ((USHORT)Rom[ATOM_STRING_START_OFFSET + 1] << 8);
    if (strOff == 0 || strOff >= Length) return NULL;
    for (i = 0; i < Count - 1 && strOff + i < Length; i++) {
        UCHAR ch = Rom[strOff + (USHORT)i];
        if (ch < ' ' || ch > 'z') break;
        Buffer[i] = ch;
    }
    if (i == 0) return NULL;
    Buffer[i] = L'\0';
    return Buffer;
}

void HardwareInfoWrite(BC250_DEVICE* Device, const UCHAR* VbiosBytes, ULONG VbiosLength)
{
    HANDLE key;
    NTSTATUS status;
    UNICODE_STRING valueName;
    ULONG memorySize;
    ULONGLONG qwMemorySize;
    WCHAR adapter[64];
    WCHAR biosStr[128];
    PCWSTR chip;
    PCWSTR bios;
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

    chip = ChipTableLookup(deviceId);
    if (chip)
        RtlStringCchPrintfW(adapter, RTL_NUMBER_OF(adapter), L"AMD %s", chip);
    else if (deviceId)
        RtlStringCchPrintfW(adapter, RTL_NUMBER_OF(adapter), L"AMD Radeon (%04X)", (ULONG)deviceId);
    else
        RtlStringCchPrintfW(adapter, RTL_NUMBER_OF(adapter), L"AMD Radeon");

    bios = VbiosPartNumber(VbiosBytes, VbiosLength, biosStr, RTL_NUMBER_OF(biosStr));

    (void)WriteString(key, L"HardwareInformation.AdapterString", adapter);
    (void)WriteString(key, L"HardwareInformation.ChipType", chip ? chip : L"AMD ASIC");
    (void)WriteString(key, L"HardwareInformation.DacType", L"Internal");
    (void)WriteString(key, L"HardwareInformation.BiosString", bios ? bios : L"AMDGPU-WDDM");

    qwMemorySize = Device->VramLength;
    memorySize = qwMemorySize > MAXULONG ? MAXULONG : (ULONG)qwMemorySize;
    RtlInitUnicodeString(&valueName, L"HardwareInformation.MemorySize");
    (void)ZwSetValueKey(key, &valueName, 0, REG_DWORD, &memorySize, sizeof(memorySize));
    RtlInitUnicodeString(&valueName, L"HardwareInformation.qwMemorySize");
    (void)ZwSetValueKey(key, &valueName, 0, REG_QWORD, &qwMemorySize, sizeof(qwMemorySize));

    ZwClose(key);
    GuardLog("hwinfo: %S bios %S memory %llu", adapter, bios ? bios : L"(none)", qwMemorySize);
}
