// Execute the actual psp.c reader against a failing filesystem at each boundary.
// Only fake file data. No device, registry or Driver Store access.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef int32_t NTSTATUS;
typedef void *HANDLE;
typedef void *PDRIVER_OBJECT;
typedef unsigned char *PUCHAR;
typedef unsigned long ULONG;
typedef uintptr_t ULONG_PTR;
typedef wchar_t WCHAR;
typedef size_t SIZE_T;
typedef struct { const WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Flags; HANDLE Root; UNICODE_STRING *Name; } OBJECT_ATTRIBUTES;
typedef struct { ULONG_PTR Information; } IO_STATUS_BLOCK;
typedef struct { struct { int64_t QuadPart; } EndOfFile; } FILE_STANDARD_INFORMATION;
typedef struct { ULONG FileAttributes; } FILE_ATTRIBUTE_TAG_INFORMATION;
enum bc250_fw_file { TEST_FILE, BC250_FILE_COUNT = 8 };
enum { PASSIVE_LEVEL, DriverDirectoryImage = 1, FileStandardInformation = 5, FileAttributeTagInformation = 35 };
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE (-1)
#define STATUS_INVALID_PARAMETER (-2)
#define STATUS_OBJECT_NAME_INVALID (-3)
#define STATUS_REPARSE_POINT_ENCOUNTERED (-4)
#define STATUS_FILE_TOO_LARGE (-5)
#define STATUS_INSUFFICIENT_RESOURCES (-6)
#define STATUS_END_OF_FILE (-7)
#define TEST_FAILURE (-8)
#define NT_SUCCESS(s) ((s) >= 0)
#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#define OBJ_KERNEL_HANDLE 0x200
#define OBJ_CASE_INSENSITIVE 0x40
#define OBJ_DONT_REPARSE 0x1000
#define FILE_READ_DATA 1
#define FILE_READ_ATTRIBUTES 0x80
#define SYNCHRONIZE 0x100000
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_SHARE_READ 1
#define FILE_OPEN 1
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#define FILE_OPEN_REPARSE_POINT 0x200000
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define POOL_FLAG_NON_PAGED 0x40
#define BC250_PSP_TAG 0x70533242
#define BC250_PSP_MAX_FILE 0x100000
#define CHECK(e) do { checks++; if (!(e)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #e); exit(1); } } while (0)
static unsigned checks;
static int irql, systemThread, fault, opens, closes, allocs, frees, queries, reads, directories;
static int64_t fileLength;
static ULONG_PTR readLength;
static ULONG fileAttributes;
static const char *fileName;
static HANDLE rootHandle = (HANDLE)(uintptr_t)1, fileHandle = (HANDLE)(uintptr_t)2;
static int KeGetCurrentIrql(void) { return irql; }
static void *PsGetCurrentThread(void) { return rootHandle; }
static int PsIsSystemThread(void *thread) { CHECK(thread == rootHandle); return systemThread; }
static NTSTATUS IoGetDriverDirectory(PDRIVER_OBJECT driver, int kind, int flags, HANDLE *out)
{
    CHECK(driver == rootHandle && kind == DriverDirectoryImage && flags == 0);
    directories++;
    if (fault == 1) return TEST_FAILURE;
    *out = rootHandle; return STATUS_SUCCESS;
}
static void ZwClose(HANDLE h) { CHECK(h == rootHandle || h == fileHandle); closes++; }
static const char *bc250_fw_file_name(enum bc250_fw_file index) { CHECK((ULONG)index < BC250_FILE_COUNT); return fileName; }
static void RtlInitUnicodeString(UNICODE_STRING *s, const WCHAR *p) { s->Buffer = p; }
#define InitializeObjectAttributes(a,n,f,r,s) do { (a)->Flags=(f); (a)->Root=(r); (a)->Name=(n); } while (0)
static NTSTATUS ZwCreateFile(HANDLE *file, ULONG access, OBJECT_ATTRIBUTES *a, IO_STATUS_BLOCK *io,
                            void *size, ULONG attr, ULONG share, ULONG how, ULONG options, void *ea, ULONG eaLength)
{
    (void)io; (void)size; (void)ea;
    opens++;
    CHECK(access == (FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE));
    CHECK(a->Root == rootHandle && a->Flags == (OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE));
    CHECK(wcslen(a->Name->Buffer) == strlen(fileName));
    CHECK(attr == FILE_ATTRIBUTE_NORMAL && share == FILE_SHARE_READ && how == FILE_OPEN && eaLength == 0);
    CHECK(options == (FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT));
    if (fault == 2) return TEST_FAILURE;
    *file = fileHandle; return STATUS_SUCCESS;
}
static NTSTATUS ZwQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *io, void *out, ULONG size, int kind)
{
    (void)io;
    queries++; CHECK(h == fileHandle);
    if (kind == FileAttributeTagInformation) {
        CHECK(size == sizeof(FILE_ATTRIBUTE_TAG_INFORMATION));
        if (fault == 3) return TEST_FAILURE;
        ((FILE_ATTRIBUTE_TAG_INFORMATION *)out)->FileAttributes = fileAttributes;
    } else {
        CHECK(kind == FileStandardInformation && size == sizeof(FILE_STANDARD_INFORMATION));
        if (fault == 4) return TEST_FAILURE;
        ((FILE_STANDARD_INFORMATION *)out)->EndOfFile.QuadPart = fileLength;
    }
    return STATUS_SUCCESS;
}
static void *ExAllocatePool2(ULONG flags, SIZE_T size, ULONG tag)
{
    CHECK(flags == POOL_FLAG_NON_PAGED && tag == BC250_PSP_TAG && size == (SIZE_T)fileLength);
    if (fault == 5) return NULL;
    allocs++; return malloc(size);
}
static void ExFreePoolWithTag(void *p, ULONG tag) { CHECK(p != NULL && tag == BC250_PSP_TAG); frees++; free(p); }
static NTSTATUS ZwReadFile(HANDLE h, void *event, void *apc, void *context, IO_STATUS_BLOCK *io,
                          PUCHAR data, ULONG size, void *offset, void *key)
{
    (void)event; (void)apc; (void)context; (void)offset; (void)key;
    reads++; CHECK(h == fileHandle && size == fileLength);
    if (fault == 6) return TEST_FAILURE;
    memset(data, 0xa5, size); io->Information = readLength;
    return STATUS_SUCCESS;
}
#include "firmware_io_actual.inc"

static void Reset(void)
{
    CHECK(allocs == frees);
    PspDriverCleanup();
    fault = irql = opens = closes = allocs = frees = queries = reads = directories = 0;
    systemThread = 1; fileAttributes = 0; fileLength = 32; readLength = 32;
    fileName = "cyan_skillfish2_ce.bin";
}
static void Refuse(NTSTATUS expected, enum bc250_fw_file index)
{
    PUCHAR data = (PUCHAR)(uintptr_t)99; ULONG size = 99;
    CHECK(ReadOneFile(index, &data, &size) == expected);
    CHECK(data == NULL && size == 0 && allocs == frees);
}
int main(void)
{
    PUCHAR data; ULONG size; int i;
    const char *badNames[] = {"../ce.bin", "dir\\ce.bin", "C:ce.bin", "stream:ce", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bin"};
    Reset(); Refuse(STATUS_INVALID_DEVICE_STATE, TEST_FILE); CHECK(opens == 0);
    irql = 2; CHECK(PspDriverInit(rootHandle) == STATUS_INVALID_DEVICE_STATE && directories == 0);
    irql = 0; systemThread = 0; CHECK(PspDriverInit(rootHandle) == STATUS_INVALID_DEVICE_STATE && directories == 0);
    systemThread = 1; fault = 1; CHECK(PspDriverInit(rootHandle) == TEST_FAILURE && g_FirmwareDirectory == NULL);
    Reset(); CHECK(PspDriverInit(rootHandle) == 0);
    irql = 2; Refuse(STATUS_INVALID_DEVICE_STATE, TEST_FILE); CHECK(opens == 0); irql = 0;
    Refuse(STATUS_INVALID_PARAMETER, (enum bc250_fw_file)-1);
    Refuse(STATUS_INVALID_PARAMETER, (enum bc250_fw_file)BC250_FILE_COUNT); CHECK(opens == 0);
    for (i = 0; i < (int)RTL_NUMBER_OF(badNames); i++) { fileName = badNames[i]; Refuse(STATUS_OBJECT_NAME_INVALID, TEST_FILE); CHECK(opens == 0); }
    for (i = 2; i <= 6; i++) {
        Reset(); CHECK(PspDriverInit(rootHandle) == 0); fault = i;
        Refuse(i == 5 ? STATUS_INSUFFICIENT_RESOURCES : TEST_FAILURE, TEST_FILE);
        CHECK(opens == 1 && closes == (i == 2 ? 0 : 1));
    }
    Reset(); CHECK(PspDriverInit(rootHandle) == 0); fileAttributes = FILE_ATTRIBUTE_REPARSE_POINT;
    Refuse(STATUS_REPARSE_POINT_ENCOUNTERED, TEST_FILE); CHECK(queries == 1 && allocs == 0 && closes == 1);
    for (i = 0; i < 3; i++) {
        Reset(); CHECK(PspDriverInit(rootHandle) == 0); fileLength = i == 0 ? 0 : (i == 1 ? -1 : BC250_PSP_MAX_FILE + 1);
        Refuse(STATUS_FILE_TOO_LARGE, TEST_FILE); CHECK(allocs == 0 && reads == 0 && closes == 1);
    }
    Reset(); CHECK(PspDriverInit(rootHandle) == 0); readLength = 31;
    Refuse(STATUS_END_OF_FILE, TEST_FILE); CHECK(allocs == 1 && frees == 1 && closes == 1);
    Reset(); CHECK(PspDriverInit(rootHandle) == 0);
    CHECK(ReadOneFile(TEST_FILE, &data, &size) == 0 && size == 32 && closes == 1);
    CHECK(data[0] == 0xa5 && data[31] == 0xa5 && allocs == 1 && frees == 0);
    ExFreePoolWithTag(data, BC250_PSP_TAG);
    PspDriverCleanup(); CHECK(closes == 2 && g_FirmwareDirectory == NULL);
    PspDriverCleanup(); CHECK(closes == 2);
    printf("firmware I/O: %u checks passed\n", checks);
    return 0;
}
