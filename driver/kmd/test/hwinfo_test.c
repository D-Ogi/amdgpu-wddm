// BD-245: host test for HardwareInfoWrite. Stubs the kernel registry and PCI
// config calls; validates that the correct REG_BINARY strings and sizes are
// written to the driver key for known and unknown device IDs.
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
typedef unsigned short USHORT;
typedef unsigned char UCHAR, BOOLEAN, *PUCHAR;
typedef void *HANDLE, *PVOID, *PDEVICE_OBJECT;
typedef const wchar_t *PCWSTR;
typedef ULONG *PULONG;
typedef unsigned long long SIZE_T;
typedef struct { long long QuadPart; } PHYSICAL_ADDRESS, LARGE_INTEGER;
typedef struct { wchar_t *Buffer; unsigned short Length, MaximumLength; } UNICODE_STRING;
#define TRUE 1
#define FALSE 0
#define MAXULONG 0xFFFFFFFFUL
#define STATUS_SUCCESS 0L
#define NT_SUCCESS(s) ((s)>=0)
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_QWORD 11
#define PLUGPLAY_REGKEY_DRIVER 2
#define KEY_SET_VALUE 2
#define DXGK_WHICHSPACE_CONFIG 0
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define _countof(a) (sizeof(a)/sizeof((a)[0]))

static unsigned checks, failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)

// Registry model: capture what HardwareInfoWrite writes.
#define MAX_VALUES 8
static struct {
    wchar_t name[80];
    ULONG type;
    UCHAR data[256];
    ULONG bytes;
} regValues[MAX_VALUES];
static unsigned regCount;
static NTSTATUS openKeyStatus;
static HANDLE fakeKey;

static void RtlInitUnicodeString(UNICODE_STRING* s, PCWSTR src) {
    s->Buffer = (wchar_t*)src;
    s->Length = (unsigned short)(wcslen(src) * sizeof(wchar_t));
    s->MaximumLength = s->Length + sizeof(wchar_t);
}
static NTSTATUS IoOpenDeviceRegistryKey(PDEVICE_OBJECT pdo, ULONG type, ULONG access, HANDLE* key) {
    (void)pdo; (void)type; (void)access;
    *key = NT_SUCCESS(openKeyStatus) ? &fakeKey : NULL;
    return openKeyStatus;
}
static NTSTATUS ZwSetValueKey(HANDLE key, UNICODE_STRING* name, ULONG idx, ULONG type, PVOID data, ULONG bytes) {
    (void)key; (void)idx;
    if (regCount >= MAX_VALUES) return (NTSTATUS)-1;
    wcsncpy_s(regValues[regCount].name, 80, name->Buffer, _TRUNCATE);
    regValues[regCount].type = type;
    if (bytes <= 256) memcpy(regValues[regCount].data, data, bytes);
    regValues[regCount].bytes = bytes;
    regCount++;
    return STATUS_SUCCESS;
}
static void ZwClose(HANDLE key) { (void)key; }

// PCI config read model
static USHORT pciDeviceId;
static NTSTATUS pciReadStatus;
typedef struct {
    HANDLE DeviceHandle;
    NTSTATUS (*DxgkCbReadDeviceSpace)(HANDLE, ULONG, void*, ULONG, ULONG, ULONG*);
} FAKE_DXGK;

static NTSTATUS FakeReadDeviceSpace(HANDLE h, ULONG space, void* buf, ULONG off, ULONG len, ULONG* read) {
    (void)h; (void)space;
    if (off == 2 && len == 2 && NT_SUCCESS(pciReadStatus)) {
        ((UCHAR*)buf)[0] = (UCHAR)(pciDeviceId & 0xFF);
        ((UCHAR*)buf)[1] = (UCHAR)(pciDeviceId >> 8);
        *read = 2;
        return STATUS_SUCCESS;
    }
    *read = 0;
    return pciReadStatus ? pciReadStatus : (NTSTATUS)-1;
}

static void GuardLog(const char* fmt, ...) { (void)fmt; }

// Minimal device structure matching hwinfo.c's needs.
typedef struct {
    PDEVICE_OBJECT PhysicalDeviceObject;
    FAKE_DXGK Dxgk;
    ULONGLONG VramLength;
} BC250_DEVICE;

/* ACTUAL_HWINFO */

static const wchar_t* FindStringValue(const wchar_t* name) {
    unsigned i;
    for (i = 0; i < regCount; i++)
        if (!wcscmp(regValues[i].name, name) && regValues[i].type == REG_BINARY)
            return (const wchar_t*)regValues[i].data;
    return NULL;
}
static int FindDword(const wchar_t* name, ULONG* out) {
    unsigned i;
    for (i = 0; i < regCount; i++)
        if (!wcscmp(regValues[i].name, name) && regValues[i].type == REG_DWORD && regValues[i].bytes == 4) {
            memcpy(out, regValues[i].data, 4);
            return 1;
        }
    return 0;
}
static int FindQword(const wchar_t* name, ULONGLONG* out) {
    unsigned i;
    for (i = 0; i < regCount; i++)
        if (!wcscmp(regValues[i].name, name) && regValues[i].type == REG_QWORD && regValues[i].bytes == 8) {
            memcpy(out, regValues[i].data, 8);
            return 1;
        }
    return 0;
}

static void ResetReg(void) { regCount = 0; memset(regValues, 0, sizeof(regValues)); }

int main(void)
{
    BC250_DEVICE dev;
    const wchar_t *s;
    ULONG mem32; ULONGLONG mem64;
    int pdo = 1;

    // Case 1: BC-250 (device 0x13FE), 8 GiB VRAM
    ResetReg();
    memset(&dev, 0, sizeof(dev));
    dev.PhysicalDeviceObject = &pdo;
    dev.Dxgk.DeviceHandle = &pdo;
    dev.Dxgk.DxgkCbReadDeviceSpace = FakeReadDeviceSpace;
    dev.VramLength = 8ull << 30;
    pciDeviceId = 0x13FE;
    pciReadStatus = STATUS_SUCCESS;
    openKeyStatus = STATUS_SUCCESS;
    HardwareInfoWrite(&dev);
    CHECK(regCount == 6);
    s = FindStringValue(L"HardwareInformation.AdapterString");
    CHECK(s && !wcscmp(s, L"AMD Cyan Skillfish"));
    s = FindStringValue(L"HardwareInformation.ChipType");
    CHECK(s && !wcscmp(s, L"Cyan Skillfish"));
    s = FindStringValue(L"HardwareInformation.DacType");
    CHECK(s && !wcscmp(s, L"Internal"));
    s = FindStringValue(L"HardwareInformation.BiosString");
    CHECK(s && !wcscmp(s, L"AMDGPU-WDDM"));
    CHECK(FindDword(L"HardwareInformation.MemorySize", &mem32) && mem32 == MAXULONG);
    CHECK(FindQword(L"HardwareInformation.qwMemorySize", &mem64) && mem64 == (8ull << 30));

    // Case 2: unknown device ID -> fallback name
    ResetReg();
    pciDeviceId = 0xBEEF;
    dev.VramLength = 4ull << 30;
    HardwareInfoWrite(&dev);
    s = FindStringValue(L"HardwareInformation.AdapterString");
    CHECK(s && !wcscmp(s, L"AMD Radeon (BEEF)"));
    s = FindStringValue(L"HardwareInformation.ChipType");
    CHECK(s && !wcscmp(s, L"AMD ASIC"));
    CHECK(FindDword(L"HardwareInformation.MemorySize", &mem32) && mem32 == MAXULONG);
    CHECK(FindQword(L"HardwareInformation.qwMemorySize", &mem64) && mem64 == (4ull << 30));

    // Case 3: no PCI read callback -> "AMD Radeon"
    ResetReg();
    dev.Dxgk.DxgkCbReadDeviceSpace = NULL;
    dev.VramLength = 0;
    HardwareInfoWrite(&dev);
    s = FindStringValue(L"HardwareInformation.AdapterString");
    CHECK(s && !wcscmp(s, L"AMD Radeon"));
    CHECK(FindDword(L"HardwareInformation.MemorySize", &mem32) && mem32 == 0);

    // Case 4: IoOpenDeviceRegistryKey fails -> no writes, no crash
    ResetReg();
    openKeyStatus = (NTSTATUS)-1;
    dev.Dxgk.DxgkCbReadDeviceSpace = FakeReadDeviceSpace;
    HardwareInfoWrite(&dev);
    CHECK(regCount == 0);

    // Case 5: NULL PDO -> no crash
    ResetReg();
    dev.PhysicalDeviceObject = NULL;
    openKeyStatus = STATUS_SUCCESS;
    HardwareInfoWrite(&dev);
    CHECK(regCount == 0);

    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
