// Durable board backup outside the replaceable driver-service key. DriverData is
// the OS symbolic link documented by the driver-isolation contract. No drive letter.
// The checksum detects corruption; protected ownership/ACL supply access control.
#include <ntifs.h>
#include "bc250kmd.h"
#include "board_memory_store.h"
#include "uma_transport.h"

C_ASSERT(sizeof(BOARD_STORE_RECORD) == 88);
C_ASSERT(sizeof(BOARD_STORE_JOURNAL) == 64);

/* Aligned fixed SIDs: S-1-5-18 and S-1-5-32-544. */
static const ULONG SystemSid[] = {0x00000101u, 0x05000000u, 18u};
static const ULONG AdminSid[] = {0x00000201u, 0x05000000u, 32u, 544u};

typedef struct BOARD_STORE_SECURITY {
    SECURITY_DESCRIPTOR Descriptor;
    union { ULONG Alignment; UCHAR Bytes[128]; } AclStorage;
} BOARD_STORE_SECURITY;

static BOOLEAN StoreIdentity(const BC250_DEVICE* device)
{
    ULONG i;
    UCHAR any = 0;
    if (!device || device->BoardMemoryMapping != BC250_UMA_MAPPING_EXTENDED_PORT ||
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
    status = RtlAddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, (PSID)SystemSid);
    if (!NT_SUCCESS(status)) return status;
    status = RtlAddAccessAllowedAce(acl, ACL_REVISION, FILE_GENERIC_READ, (PSID)AdminSid);
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
        ULONG mask = i == 0 ? FILE_ALL_ACCESS : FILE_GENERIC_READ;
        if (!NT_SUCCESS(RtlGetAce(acl, i, (PVOID*)&ace)) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            ace->Header.AceFlags != 0 || ace->Mask != mask ||
            ace->Header.AceSize != FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(wanted) ||
            !RtlEqualSid((PSID)&ace->SidStart, wanted)) return FALSE;
    }
    return TRUE;
}
typedef struct BOARD_DIRECTORY {
    HANDLE DriverData, Company, Board;
} BOARD_DIRECTORY;

static NTSTATUS CloseDirectory(BOARD_DIRECTORY* directory)
{
    NTSTATUS status = STATUS_SUCCESS;
    if (directory->Board && !NT_SUCCESS(ZwClose(directory->Board))) status = STATUS_UNSUCCESSFUL;
    if (directory->Company && !NT_SUCCESS(ZwClose(directory->Company))) status = STATUS_UNSUCCESSFUL;
    if (directory->DriverData && !NT_SUCCESS(ZwClose(directory->DriverData))) status = STATUS_UNSUCCESSFUL;
    RtlZeroMemory(directory, sizeof(*directory));
    return status;
}
static NTSTATUS CheckObject(HANDLE file, BOOLEAN directory, BOOLEAN secure)
{
    FILE_ATTRIBUTE_TAG_INFORMATION tag;
    IO_STATUS_BLOCK io;
    NTSTATUS status = ZwQueryInformationFile(file, &io, &tag, sizeof(tag), FileAttributeTagInformation);
    if (!NT_SUCCESS(status)) return status;
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != !!directory) return STATUS_REPARSE_POINT_ENCOUNTERED;
    if (secure && !StoreSecurityValid(file)) return STATUS_ACCESS_DENIED;
    return STATUS_SUCCESS;
}
static NTSTATUS OpenChild(HANDLE parent, PCWSTR name, BOOLEAN create, BOOLEAN directory,
                          BOOLEAN write, HANDLE* out)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attributes;
    BOARD_STORE_SECURITY security;
    IO_STATUS_BLOCK io;
    HANDLE handle = NULL;
    NTSTATUS status;
    ULONG access = FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
    ULONG options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
        (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    // The root symbolic link is OS-owned. All company paths below it are fixed,
    // relative names, opened without following filesystem reparses.
    *out = NULL;
    if (write) { access |= directory ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : FILE_WRITE_DATA; options |= FILE_WRITE_THROUGH; }
    status = StoreSecurity(&security);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&path, name);
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE |
        (parent ? OBJ_DONT_REPARSE : 0), parent, create ? &security.Descriptor : NULL);
    status = ZwCreateFile(&handle, access, &attributes, &io, NULL,
        directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, create ? (directory ? FILE_OPEN_IF : FILE_CREATE) : FILE_OPEN, options, NULL, 0);
    if (!NT_SUCCESS(status)) return status;
    status = CheckObject(handle, directory, parent != NULL);
    if (!NT_SUCCESS(status)) { ZwClose(handle); return status; }
    *out = handle;
    return STATUS_SUCCESS;
}
static NTSTATUS OpenDirectory(BOARD_DIRECTORY* directory, BOOLEAN create, BOOLEAN write)
{
    NTSTATUS status;
    RtlZeroMemory(directory, sizeof(*directory));
    status = OpenChild(NULL, L"\\DriverData", FALSE, TRUE, FALSE, &directory->DriverData);
    if (NT_SUCCESS(status)) status = OpenChild(directory->DriverData, L"amdgpu-wddm", create, TRUE, write, &directory->Company);
    if (NT_SUCCESS(status)) status = OpenChild(directory->Company, L"BoardMemory", create, TRUE, write, &directory->Board);
    if (!NT_SUCCESS(status)) CloseDirectory(directory);
    return status;
}
static NTSTATUS ReadRecord(HANDLE file, void* data, ULONG bytes)
{
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION info;
    LARGE_INTEGER offset = {0};
    NTSTATUS status = ZwQueryInformationFile(file, &io, &info, sizeof(info), FileStandardInformation);
    if (!NT_SUCCESS(status)) return status;
    if (info.EndOfFile.QuadPart != bytes) return STATUS_INFO_LENGTH_MISMATCH;
    status = ZwReadFile(file, NULL, NULL, NULL, &io, data, bytes, &offset, NULL);
    if (NT_SUCCESS(status) && io.Information != bytes) status = STATUS_END_OF_FILE;
    return status;
}
static NTSTATUS LoadRecord(HANDLE directory, PCWSTR name, void* data, ULONG bytes)
{
    HANDLE file;
    NTSTATUS status = OpenChild(directory, name, FALSE, FALSE, FALSE, &file);
    if (!NT_SUCCESS(status)) return status;
    status = ReadRecord(file, data, bytes);
    if (!NT_SUCCESS(ZwClose(file))) status = STATUS_UNSUCCESSFUL;
    return status;
}
static NTSTATUS SaveRecord(HANDLE directory, PCWSTR name, const void* data, ULONG bytes, BOOLEAN create)
{
    UCHAR readback[sizeof(BOARD_STORE_RECORD)];
    IO_STATUS_BLOCK io;
    LARGE_INTEGER offset = {0};
    HANDLE file;
    NTSTATUS status;
    if (bytes > sizeof(readback)) return STATUS_INVALID_BUFFER_SIZE;
    status = OpenChild(directory, name, create, FALSE, TRUE, &file);
    if (!NT_SUCCESS(status)) return status;
    status = ZwWriteFile(file, NULL, NULL, NULL, &io, (PVOID)data, bytes, &offset, NULL);
    if (NT_SUCCESS(status) && io.Information != bytes) status = STATUS_DISK_FULL;
    if (NT_SUCCESS(status)) status = ZwFlushBuffersFile(file, &io);
    if (NT_SUCCESS(status)) status = ReadRecord(file, readback, bytes);
    if (NT_SUCCESS(status) && RtlCompareMemory(data, readback, bytes) != bytes) status = STATUS_DATA_ERROR;
    if (!NT_SUCCESS(ZwClose(file))) status = STATUS_UNSUCCESSFUL;
    return status;
}
static BOOLEAN JournalValid(const BC250_DEVICE* device, const BOARD_STORE_JOURNAL* journal)
{
    return journal->Magic == BOARD_JOURNAL_MAGIC && journal->Version == BOARD_JOURNAL_VERSION &&
        journal->Bytes == sizeof(*journal) && journal->Sequence != 0 &&
        journal->State <= BOARD_JOURNAL_PENDING && journal->Mapping == device->BoardMemoryMapping &&
        journal->BiosId == device->BoardMemoryBiosId &&
        RtlCompareMemory(journal->MachineId, device->BoardMemoryMachineId, 16) == 16 &&
        journal->Reserved[0] == 0 && journal->Reserved[1] == 0 && journal->Checksum == JournalChecksum(journal);
}
// 1 original+current journal, 0 explicitly seeded empty store, -1 unknown/error.
// Never fall back to an older journal. Missing or torn current state is a lockout.
static int ReadPair(HANDLE directory, BC250_DEVICE* device, BOARD_STORE_RECORD* record, BOARD_STORE_JOURNAL* journal)
{
    NTSTATUS status = LoadRecord(directory, L"journal.dat", journal, sizeof(*journal));
    if (!NT_SUCCESS(status) || !JournalValid(device, journal)) return -1;
    status = LoadRecord(directory, L"original.dat", record, sizeof(*record));
    if (status == STATUS_OBJECT_NAME_NOT_FOUND && journal->State == BOARD_JOURNAL_EMPTY && journal->OriginalChecksum == 0) return 0;
    if (!NT_SUCCESS(status) || !StoreRecordValid(device, record) || journal->State == BOARD_JOURNAL_EMPTY ||
        journal->OriginalChecksum != record->Checksum) return -1;
    return 1;
}
static void InitializeJournal(BC250_DEVICE* device, BOARD_STORE_JOURNAL* journal)
{
    RtlZeroMemory(journal, sizeof(*journal));
    journal->Magic = BOARD_JOURNAL_MAGIC; journal->Version = BOARD_JOURNAL_VERSION;
    journal->Bytes = sizeof(*journal); journal->Sequence = 1;
    journal->Mapping = device->BoardMemoryMapping; journal->BiosId = device->BoardMemoryBiosId;
    RtlCopyMemory(journal->MachineId, device->BoardMemoryMachineId, 16);
}
static int StoreLoad(void* context, unsigned char backup[28], unsigned* pending)
{
    BC250_DEVICE* device = context;
    BOARD_DIRECTORY directory;
    BOARD_STORE_RECORD record;
    BOARD_STORE_JOURNAL journal;
    int found;
    if (!backup || !pending) return -1;
    RtlZeroMemory(backup, 28); *pending = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !StoreIdentity(device) ||
        !NT_SUCCESS(OpenDirectory(&directory, FALSE, FALSE))) return -1;
    found = ReadPair(directory.Board, device, &record, &journal);
    if (!NT_SUCCESS(CloseDirectory(&directory))) return -1;
    if (found == 1) {
        RtlCopyMemory(backup, record.Backup, 28);
        *pending = journal.State == BOARD_JOURNAL_PENDING;
    }
    return found;
}
static int StoreSave(void* context, const unsigned char backup[28], unsigned pending)
{
    BC250_DEVICE* device = context;
    BOARD_DIRECTORY directory;
    BOARD_STORE_RECORD record;
    BOARD_STORE_JOURNAL journal;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    int found;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !StoreIdentity(device) || pending > 1 ||
        !bc250_uma_validate(backup) || !NT_SUCCESS(OpenDirectory(&directory, FALSE, TRUE))) return 0;
    found = ReadPair(directory.Board, device, &record, &journal);
    if (found < 0 || journal.Sequence == ~0ull) goto done;
    if (found == 0) {
        RtlZeroMemory(&record, sizeof(record));
        record.Magic = BOARD_STORE_MAGIC; record.Version = BOARD_STORE_VERSION; record.Bytes = sizeof(record);
        record.Mapping = device->BoardMemoryMapping; record.BiosId = device->BoardMemoryBiosId;
        RtlCopyMemory(record.MachineId, device->BoardMemoryMachineId, 16);
        RtlCopyMemory(record.Backup, backup, 28); record.Checksum = StoreChecksum(&record);
        // FILE_CREATE: never overwrite an existing original, even after a fault.
        status = SaveRecord(directory.Board, L"original.dat", &record, sizeof(record), TRUE);
        if (!NT_SUCCESS(status)) goto done;
    } else if (RtlCompareMemory(record.Backup, backup, 28) != 28) goto done;
    journal.OriginalChecksum = record.Checksum;
    journal.State = pending ? BOARD_JOURNAL_PENDING : BOARD_JOURNAL_CLEAR;
    journal.Sequence++; journal.Checksum = JournalChecksum(&journal);
    status = SaveRecord(directory.Board, L"journal.dat", &journal, sizeof(journal), FALSE);
done:
    if (!NT_SUCCESS(CloseDirectory(&directory))) status = STATUS_UNSUCCESSFUL;
    return NT_SUCCESS(status);
}

NTSTATUS BoardMemoryStoreSeed(BC250_DEVICE* device, const BOARD_STORE_RECORD* legacy, BOOLEAN fresh)
{
    BOARD_DIRECTORY directory;
    BOARD_STORE_RECORD record;
    BOARD_STORE_JOURNAL journal;
    NTSTATUS status, oldStatus;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !PsIsSystemThread(PsGetCurrentThread()) || !StoreIdentity(device))
        return STATUS_INVALID_DEVICE_STATE;
    if ((!legacy && !fresh) || (legacy && (fresh || !StoreRecordValid(device, legacy)))) return STATUS_DATA_ERROR;
    status = OpenDirectory(&directory, TRUE, TRUE);
    if (!NT_SUCCESS(status)) return status;
    status = LoadRecord(directory.Board, L"journal.dat", &journal, sizeof(journal));
    oldStatus = LoadRecord(directory.Board, L"original.dat", &record, sizeof(record));
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND) goto done;
    if (!NT_SUCCESS(oldStatus) && oldStatus != STATUS_OBJECT_NAME_NOT_FOUND) { status = oldStatus; goto done; }
    if (status != STATUS_OBJECT_NAME_NOT_FOUND || oldStatus != STATUS_OBJECT_NAME_NOT_FOUND) {
        // Idempotence only for a complete state of the same identity. A damaged
        // existing generation is never re-seeded from a stale migration export.
        int pair = ReadPair(directory.Board, device, &record, &journal);
        status = pair < 0 || (legacy && (pair != 1 || RtlCompareMemory(legacy, &record, sizeof(record)) != sizeof(record)))
            ? STATUS_DATA_ERROR : STATUS_SUCCESS;
        goto done;
    }
    InitializeJournal(device, &journal);
    if (legacy) {
        status = SaveRecord(directory.Board, L"original.dat", legacy, sizeof(*legacy), TRUE);
        if (!NT_SUCCESS(status)) goto done;
        journal.OriginalChecksum = legacy->Checksum;
        journal.State = legacy->Pending ? BOARD_JOURNAL_PENDING : BOARD_JOURNAL_CLEAR;
    }
    journal.Checksum = JournalChecksum(&journal);
    status = SaveRecord(directory.Board, L"journal.dat", &journal, sizeof(journal), TRUE);
done:
    if (!NT_SUCCESS(CloseDirectory(&directory))) status = STATUS_UNSUCCESSFUL;
    return status;
}
void BoardMemoryStoreIo(BC250_DEVICE* device, struct board_memory_store* out)
{
    if (!out) return;
    out->context = device; out->load = StoreLoad; out->save = StoreSave;
}
