// Durable board backup outside the replaceable driver-service key.
// The checksum detects corruption; protected ownership/ACL supply access control.
#include <ntifs.h>
#include "bc250kmd.h"
#include "board_memory_store.h"

#define BOARD_STORE_MAGIC 0x314d4241u
#define BOARD_STORE_VERSION 1u
#define BOARD_STORE_PATH L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\amdgpu-wddm-board-memory"
#define BOARD_STORE_VALUE L"Backup"

typedef struct BOARD_STORE_RECORD {
    ULONG Magic, Version, Bytes, Mapping, BiosId;
    UCHAR MachineId[16];
    UCHAR Backup[28];
    ULONG Pending, Checksum, Reserved[4];
} BOARD_STORE_RECORD;
C_ASSERT(sizeof(BOARD_STORE_RECORD) == 88);

/* Aligned fixed SIDs: S-1-5-18 and S-1-5-32-544. */
static const ULONG SystemSid[] = {0x00000101u, 0x05000000u, 18u};
static const ULONG AdminSid[] = {0x00000201u, 0x05000000u, 32u, 544u};

typedef struct BOARD_STORE_SECURITY {
    SECURITY_DESCRIPTOR Descriptor;
    union { ULONG Alignment; UCHAR Bytes[128]; } AclStorage;
} BOARD_STORE_SECURITY;

static ULONG StoreChecksum(const BOARD_STORE_RECORD* record)
{
    BOARD_STORE_RECORD copy = *record;
    const UCHAR* bytes = (const UCHAR*)&copy;
    ULONG value = 2166136261u, i;
    copy.Checksum = 0;
    for (i = 0; i < sizeof(copy); ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}
static BOOLEAN StoreIdentity(const BC250_DEVICE* device)
{
    ULONG i;
    UCHAR any = 0;
    if (!device || device->BoardMemoryMapping < 1 || device->BoardMemoryMapping > 2 ||
        device->BoardMemoryBiosId == 0) return FALSE;
    for (i = 0; i < 16; ++i) any |= device->BoardMemoryMachineId[i];
    return any != 0;
}
static BOOLEAN StoreRecordValid(const BC250_DEVICE* device, const BOARD_STORE_RECORD* record)
{
    ULONG i;
    if (record->Magic != BOARD_STORE_MAGIC || record->Version != BOARD_STORE_VERSION ||
        record->Bytes != sizeof(*record) || record->Mapping != device->BoardMemoryMapping ||
        record->BiosId != device->BoardMemoryBiosId || record->Pending > 1 ||
        RtlCompareMemory(record->MachineId, device->BoardMemoryMachineId, 16) != 16 ||
        !bc250_uma_validate(record->Backup) || record->Checksum != StoreChecksum(record)) return FALSE;
    for (i = 0; i < RTL_NUMBER_OF(record->Reserved); ++i) if (record->Reserved[i]) return FALSE;
    return TRUE;
}
static NTSTATUS StoreSecurity(BOARD_STORE_SECURITY* security)
{
    PACL acl = (PACL)security->AclStorage.Bytes;
    NTSTATUS status;
    RtlZeroMemory(security, sizeof(*security));
    status = RtlCreateSecurityDescriptor(&security->Descriptor, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(status)) return status;
    status = RtlCreateAcl(acl, sizeof(security->AclStorage.Bytes), ACL_REVISION);
    if (!NT_SUCCESS(status)) return status;
    status = RtlAddAccessAllowedAce(acl, ACL_REVISION, KEY_ALL_ACCESS, (PSID)SystemSid);
    if (!NT_SUCCESS(status)) return status;
    status = RtlAddAccessAllowedAce(acl, ACL_REVISION, KEY_READ, (PSID)AdminSid);
    if (!NT_SUCCESS(status)) return status;
    status = RtlSetDaclSecurityDescriptor(&security->Descriptor, TRUE, acl, FALSE);
    if (!NT_SUCCESS(status)) return status;
    status = RtlSetOwnerSecurityDescriptor(&security->Descriptor, (PSID)SystemSid, FALSE);
    if (NT_SUCCESS(status)) security->Descriptor.Control |= SE_DACL_PROTECTED;
    return status;
}
static BOOLEAN StoreSecurityValid(HANDLE key)
{
    union { ULONG Alignment; UCHAR Bytes[512]; } buffer;
    PSECURITY_DESCRIPTOR descriptor = (PSECURITY_DESCRIPTOR)buffer.Bytes;
    PSID owner;
    PACL acl;
    BOOLEAN defaulted, present;
    ULONG needed, i;
    NTSTATUS status = ZwQuerySecurityObject(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                            descriptor, sizeof(buffer.Bytes), &needed);
    if (!NT_SUCCESS(status) || needed > sizeof(buffer.Bytes) ||
        !RtlValidSecurityDescriptor(descriptor) || !(((SECURITY_DESCRIPTOR*)descriptor)->Control & SE_DACL_PROTECTED)) return FALSE;
    if (!NT_SUCCESS(RtlGetOwnerSecurityDescriptor(descriptor, &owner, &defaulted)) ||
        !owner || !RtlEqualSid(owner, (PSID)SystemSid)) return FALSE;
    if (!NT_SUCCESS(RtlGetDaclSecurityDescriptor(descriptor, &present, &acl, &defaulted)) ||
        !present || !acl || acl->AceCount != 2) return FALSE;
    for (i = 0; i < 2; ++i) {
        ACCESS_ALLOWED_ACE* ace;
        PSID wanted = i == 0 ? (PSID)SystemSid : (PSID)AdminSid;
        ULONG mask = i == 0 ? KEY_ALL_ACCESS : KEY_READ;
        if (!NT_SUCCESS(RtlGetAce(acl, i, (PVOID*)&ace)) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            ace->Header.AceFlags != 0 || ace->Mask != mask ||
            ace->Header.AceSize != FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(wanted) ||
            !RtlEqualSid((PSID)&ace->SidStart, wanted)) return FALSE;
    }
    return TRUE;
}
/* 1 found, 0 absent, -1 inaccessible/malformed. */
static int StoreRead(HANDLE key, BC250_DEVICE* device, BOARD_STORE_RECORD* record)
{
    union { ULONG Alignment; UCHAR Bytes[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(BOARD_STORE_RECORD)]; } buffer;
    PKEY_VALUE_PARTIAL_INFORMATION value = (PKEY_VALUE_PARTIAL_INFORMATION)buffer.Bytes;
    UNICODE_STRING name = RTL_CONSTANT_STRING(BOARD_STORE_VALUE);
    ULONG needed = 0;
    NTSTATUS status = ZwQueryValueKey(key, &name, KeyValuePartialInformation, value, sizeof(buffer.Bytes), &needed);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) return 0;
    if (!NT_SUCCESS(status) || value->Type != REG_BINARY || value->DataLength != sizeof(*record) ||
        needed != FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(*record)) return -1;
    RtlCopyMemory(record, value->Data, sizeof(*record));
    return StoreRecordValid(device, record) ? 1 : -1;
}
static int StoreLoad(void* context, unsigned char backup[28], unsigned* pending)
{
    BC250_DEVICE* device = context;
    UNICODE_STRING path = RTL_CONSTANT_STRING(BOARD_STORE_PATH);
    OBJECT_ATTRIBUTES attributes;
    BOARD_STORE_RECORD record = {0};
    HANDLE key;
    NTSTATUS status;
    int found;
    if (!backup || !pending) return -1;
    RtlZeroMemory(backup, 28); *pending = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !StoreIdentity(device)) return -1;
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_QUERY_VALUE | READ_CONTROL, &attributes);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND) return 0;
    if (!NT_SUCCESS(status)) return -1;
    found = StoreSecurityValid(key) ? StoreRead(key, device, &record) : -1;
    if (!NT_SUCCESS(ZwClose(key))) return -1;
    if (found == 1) { RtlCopyMemory(backup, record.Backup, 28); *pending = record.Pending; }
    return found;
}
static int StoreSave(void* context, const unsigned char backup[28], unsigned pending)
{
    BC250_DEVICE* device = context;
    UNICODE_STRING path = RTL_CONSTANT_STRING(BOARD_STORE_PATH);
    UNICODE_STRING name = RTL_CONSTANT_STRING(BOARD_STORE_VALUE);
    OBJECT_ATTRIBUTES attributes;
    BOARD_STORE_SECURITY security;
    BOARD_STORE_RECORD record, previous, readback;
    HANDLE key;
    ULONG disposition;
    NTSTATUS status;
    int found, ok = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !StoreIdentity(device) || pending > 1 ||
        !bc250_uma_validate(backup)) return 0;
    status = StoreSecurity(&security);
    if (!NT_SUCCESS(status)) return 0;
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, &security.Descriptor);
    status = ZwCreateKey(&key, KEY_QUERY_VALUE | KEY_SET_VALUE | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                         &attributes, 0, NULL, REG_OPTION_NON_VOLATILE, &disposition);
    if (!NT_SUCCESS(status)) return 0;
    status = ZwSetSecurityObject(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                PROTECTED_DACL_SECURITY_INFORMATION, &security.Descriptor);
    if (!NT_SUCCESS(status) || !StoreSecurityValid(key)) goto done;
    found = StoreRead(key, device, &previous);
    if (found < 0 || (found == 1 && RtlCompareMemory(previous.Backup, backup, 28) != 28)) goto done;
    RtlZeroMemory(&record, sizeof(record));
    record.Magic = BOARD_STORE_MAGIC; record.Version = BOARD_STORE_VERSION; record.Bytes = sizeof(record);
    record.Mapping = device->BoardMemoryMapping; record.BiosId = device->BoardMemoryBiosId;
    RtlCopyMemory(record.MachineId, device->BoardMemoryMachineId, 16);
    RtlCopyMemory(record.Backup, backup, 28); record.Pending = pending;
    record.Checksum = StoreChecksum(&record);
    status = ZwSetValueKey(key, &name, 0, REG_BINARY, &record, sizeof(record));
    if (!NT_SUCCESS(status) || !NT_SUCCESS(ZwFlushKey(key))) goto done;
    if (StoreRead(key, device, &readback) != 1 || RtlCompareMemory(&record, &readback, sizeof(record)) != sizeof(record)) goto done;
    ok = 1;
done:
    if (!NT_SUCCESS(ZwClose(key))) ok = 0;
    return ok;
}
void BoardMemoryStoreIo(BC250_DEVICE* device, struct board_memory_store* out)
{
    if (!out) return;
    out->context = device; out->load = StoreLoad; out->save = StoreSave;
}
