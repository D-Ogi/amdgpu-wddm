/* Production persistence and allocation-failure paths, with fake kernel APIs.
 * No registry, security descriptor or hardware on the host is accessed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "hang_recovery.h"
#include "wave_snapshot.h"
typedef unsigned long ULONG;
typedef unsigned char UCHAR, BOOLEAN;
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
typedef void *HANDLE, *PVOID, *PSID;
typedef const wchar_t *PCWSTR;
typedef struct { PCWSTR Buffer; } UNICODE_STRING;
typedef struct { long long QuadPart; } LARGE_INTEGER;
typedef struct { unsigned Control; } SECURITY_DESCRIPTOR;
typedef struct { ULONG count, masks[2]; } ACL, *PACL;
typedef struct { UNICODE_STRING *name; HANDLE parent; SECURITY_DESCRIPTOR *security; } OBJECT_ATTRIBUTES;
typedef struct { int unused; } BC250_DEVICE;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0L
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009aL)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define KEY_READ 0x20019u
#define KEY_WRITE 0x20006u
#define KEY_ALL_ACCESS 0xf003fu
#define DELETE 0x10000u
#define WRITE_DAC 0x40000u
#define WRITE_OWNER 0x80000u
#define OBJ_KERNEL_HANDLE 0x200u
#define OBJ_CASE_INSENSITIVE 0x40u
#define REG_OPTION_NON_VOLATILE 0u
#define REG_DWORD 4u
#define REG_QWORD 11u
#define REG_BINARY 3u
#define SECURITY_DESCRIPTOR_REVISION 1u
#define ACL_REVISION 2u
#define SE_DACL_PROTECTED 0x1000u
#define OWNER_SECURITY_INFORMATION 1u
#define DACL_SECURITY_INFORMATION 4u
#define PROTECTED_DACL_SECURITY_INFORMATION 0x80000000u
#define POOL_FLAG_NON_PAGED 0x40u
#define BC250_GFX_TAG 0x1234u
#define BC250_KMD_VERSION 30u
#define RtlZeroMemory(p,n) memset(p,0,n)
static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %u: %s\n",__LINE__,#c); } } while(0)
static int DriverStateReady(void){return 1;}
static ULONG pending, persistedPending, attempts, verdict, adminMask, systemMask;
static unsigned flushes, writes, dataOrder, clearOrder, handles, allocations, captures;
static int irql, failSecurity, failData, failMarker, failFlush, failAllocate, failOpen, failChild;
static BC250_WAVE_HEADER savedHeader, persistedHeader;
static ULONGLONG interruptTime=123456789;
static int KeGetCurrentIrql(void) { return irql; }
static ULONGLONG KeQueryInterruptTime(void) { return interruptTime; }
static void KeQuerySystemTime(LARGE_INTEGER *t) { t->QuadPart=900000000; }
static void RtlInitUnicodeString(UNICODE_STRING *s,PCWSTR p) { s->Buffer=p; }
static void InitializeObjectAttributes(OBJECT_ATTRIBUTES *o,UNICODE_STRING *n,ULONG f,HANDLE h,SECURITY_DESCRIPTOR *s)
{ (void)f; o->name=n; o->parent=h; o->security=s; }
static void GuardLog(const char *format,...) { (void)format; }
static NTSTATUS OpenParameters(HANDLE *key)
{ if(failOpen)return STATUS_UNSUCCESSFUL; *key=(HANDLE)(uintptr_t)1; ++handles; return STATUS_SUCCESS; }
static NTSTATUS ZwCreateKey(HANDLE *key,ULONG access,OBJECT_ATTRIBUTES *a,ULONG t,PVOID c,ULONG option,PVOID d)
{
    (void)t; (void)c; (void)option; (void)d;
    if(!wcscmp(a->name->Buffer,L"HangRecovery")) {
        CHECK(a->parent==(HANDLE)(uintptr_t)1 && access==(KEY_READ|KEY_WRITE));
        *key=(HANDLE)(uintptr_t)2;
    } else {
        CHECK(!wcscmp(a->name->Buffer,L"Waves") && a->parent==(HANDLE)(uintptr_t)2);
        CHECK(access==(KEY_READ|KEY_WRITE|WRITE_DAC|WRITE_OWNER));
        CHECK(a->security && (a->security->Control&SE_DACL_PROTECTED));
        if(failChild)return STATUS_UNSUCCESSFUL;
        *key=(HANDLE)(uintptr_t)3;
    }
    ++handles; return STATUS_SUCCESS;
}
static void ZwClose(HANDLE key) { (void)key; CHECK(handles>0); --handles; }
static NTSTATUS ZwSetValueKey(HANDLE key,UNICODE_STRING *name,ULONG title,ULONG type,PVOID data,ULONG bytes)
{
    (void)title; ++writes;
    if(!wcscmp(name->Buffer,L"WavesPending")) {
        CHECK(key==(HANDLE)(uintptr_t)2 && type==REG_DWORD && bytes==sizeof(ULONG));
        if(failMarker)return STATUS_UNSUCCESSFUL;
        pending=*(ULONG*)data; if(!pending)clearOrder=writes;
    } else if(!wcscmp(name->Buffer,L"Snapshot")) {
        CHECK(key==(HANDLE)(uintptr_t)3 && type==REG_BINARY && bytes>=sizeof(savedHeader));
        if(failData)return STATUS_UNSUCCESSFUL;
        savedHeader=*(BC250_WAVE_HEADER*)data; dataOrder=writes;
    } else if(!wcscmp(name->Buffer,L"LastVerdict")) verdict=*(ULONG*)data;
    return STATUS_SUCCESS;
}
static NTSTATUS ZwFlushKey(HANDLE key)
{
    (void)key; ++flushes; if(failFlush)return STATUS_UNSUCCESSFUL;
    persistedPending=pending; persistedHeader=savedHeader; return STATUS_SUCCESS;
}
static NTSTATUS AddOne(HANDLE key,PCWSTR name)
{ (void)key; if(!wcscmp(name,L"Attempts"))++attempts; return STATUS_SUCCESS; }
static NTSTATUS RtlCreateSecurityDescriptor(SECURITY_DESCRIPTOR *d,ULONG revision)
{ (void)revision; memset(d,0,sizeof(*d)); return STATUS_SUCCESS; }
static NTSTATUS RtlCreateAcl(PACL acl,ULONG size,ULONG revision)
{ (void)size; (void)revision; acl->count=0; return STATUS_SUCCESS; }
static NTSTATUS RtlAddAccessAllowedAce(PACL acl,ULONG revision,ULONG mask,PSID sid)
{
    ULONG *parts=(ULONG*)sid; (void)revision;
    CHECK(acl->count<2); acl->masks[acl->count++]=mask;
    if(parts[2]==18)systemMask=mask;
    else { CHECK(parts[2]==32 && parts[3]==544); adminMask=mask; }
    return STATUS_SUCCESS;
}
static NTSTATUS RtlSetDaclSecurityDescriptor(SECURITY_DESCRIPTOR *d,BOOLEAN present,PACL acl,BOOLEAN def)
{ (void)d; CHECK(present && !def && acl->count==2); return STATUS_SUCCESS; }
static NTSTATUS RtlSetOwnerSecurityDescriptor(SECURITY_DESCRIPTOR *d,PSID sid,BOOLEAN def)
{ (void)d; CHECK(((ULONG*)sid)[2]==18 && !def); return STATUS_SUCCESS; }
static NTSTATUS ZwSetSecurityObject(HANDLE key,ULONG info,SECURITY_DESCRIPTOR *d)
{
    CHECK(key==(HANDLE)(uintptr_t)3 && info==(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION));
    CHECK(d->Control&SE_DACL_PROTECTED); return failSecurity?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
}
static PVOID ExAllocatePool2(ULONGLONG flags,size_t bytes,ULONG tag)
{ CHECK(flags==POOL_FLAG_NON_PAGED && tag==BC250_GFX_TAG); if(failAllocate)return NULL; ++allocations; return calloc(1,bytes); }
static void ExFreePoolWithTag(PVOID p,ULONG tag) { CHECK(tag==BC250_GFX_TAG && allocations); --allocations; free(p); }
static NTSTATUS GfxCaptureWaves(BC250_DEVICE *d,BC250_WAVE_SNAPSHOT *s)
{
    (void)d; ++captures; CHECK(persistedPending==1);
    s->Header.AbiVersion=BC250_WAVE_ABI_VERSION; s->Header.Bytes=sizeof(s->Header);
    s->Header.StartTime100ns=interruptTime; s->Header.EndTime100ns=interruptTime+1;
    s->Header.Result=STATUS_SUCCESS; s->Header.Flags=BC250_WAVE_COMPLETE; return STATUS_SUCCESS;
}
/* ACTUAL_SOURCE */
static void Reset(void)
{
    CHECK(handles==0 && allocations==0);
    pending=persistedPending=attempts=verdict=adminMask=systemMask=0;
    flushes=writes=dataOrder=clearOrder=captures=0;
    irql=failSecurity=failData=failMarker=failFlush=failAllocate=failOpen=failChild=0;
    memset(&savedHeader,0,sizeof(savedHeader)); memset(&persistedHeader,0,sizeof(persistedHeader));
}
static void Pending(void) { GuardRecordHangRecovery(BC250_HANG_VERDICT_PENDING,7,11,0,0,TRUE); }
int main(void)
{
    BC250_DEVICE device={0}; BC250_WAVE_HEADER h={0}; unsigned before;
    h.AbiVersion=BC250_WAVE_ABI_VERSION; h.Bytes=sizeof(h); h.Kind=BC250_WAVE_KIND_PRE_KILL;
    Reset(); Pending(); CHECK(pending==1 && persistedPending==1 && attempts==1 && flushes==1);
    GfxRecordWavesBeforeKill(&device,7,11,3);
    CHECK(captures==1 && persistedPending==0 && flushes==2 && dataOrder<clearOrder);
    CHECK(persistedHeader.Seq==7 && persistedHeader.Fence==11 && persistedHeader.Vmid==3);
    CHECK(adminMask==(KEY_READ|DELETE) && !(adminMask&2) && systemMask==KEY_ALL_ACCESS);
    CHECK(handles==0 && allocations==0);
    Reset(); Pending(); failData=1;
    CHECK(!NT_SUCCESS(GuardRecordWaveSnapshot(&h)) && persistedPending==1 && pending==1 && flushes==1);
    GuardRecordHangRecovery(BC250_HANG_VERDICT_NOT_DRAINED,7,11,2,10,FALSE);
    CHECK(persistedPending==1); /* a final verdict preserves the incomplete capture */
    failData=0; GuardRecordHangRecovery(BC250_HANG_VERDICT_PENDING,8,12,0,0,FALSE);
    CHECK(persistedPending==0); /* switch off never presents old capture as current */
    Reset(); Pending(); failSecurity=1;
    CHECK(!NT_SUCCESS(GuardRecordWaveSnapshot(&h)) && pending==1 && dataOrder==0 && flushes==1);
    Reset(); Pending(); failMarker=1;
    CHECK(!NT_SUCCESS(GuardRecordWaveSnapshot(&h)) && pending==1 && dataOrder>0 && clearOrder==0);
    Reset(); Pending(); failFlush=1;
    CHECK(!NT_SUCCESS(GuardRecordWaveSnapshot(&h)) && persistedPending==1);
    Reset(); Pending(); failChild=1;
    CHECK(!NT_SUCCESS(GuardRecordWaveSnapshot(&h)) && pending==1 && handles==0);
    Reset(); Pending(); failAllocate=1;
    GfxRecordWavesBeforeKill(&device,8,12,4);
    CHECK(captures==0 && persistedPending==0 && persistedHeader.Seq==8);
    CHECK(persistedHeader.Result==STATUS_INSUFFICIENT_RESOURCES && persistedHeader.Flags==BC250_WAVE_READ_ERROR);
    CHECK(persistedHeader.StartTime100ns==interruptTime && persistedHeader.EndTime100ns==interruptTime);
    CHECK(persistedHeader.Bytes==sizeof(h) && persistedHeader.WaveCount==0);
    Reset(); h.Kind=BC250_WAVE_KIND_LIVE;
    CHECK(GuardRecordWaveSnapshot(&h)==STATUS_INVALID_PARAMETER && writes==0 && handles==0);
    h.Kind=BC250_WAVE_KIND_PRE_KILL; h.Bytes++;
    CHECK(GuardRecordWaveSnapshot(&h)==STATUS_INVALID_PARAMETER && writes==0);
    h.Bytes--; h.WaveCount=BC250_WAVE_MAX_RECORDS+1;
    CHECK(GuardRecordWaveSnapshot(&h)==STATUS_INVALID_PARAMETER && writes==0);
    h.WaveCount=0; h.AbiVersion++;
    CHECK(GuardRecordWaveSnapshot(&h)==STATUS_INVALID_PARAMETER && writes==0);
    h.AbiVersion--; irql=2; before=writes;
    Pending(); GfxRecordWavesBeforeKill(&device,1,2,3);
    CHECK(GuardRecordWaveSnapshot(&h)==STATUS_INVALID_DEVICE_STATE && writes==before && captures==0);
    Reset(); failOpen=1; Pending(); CHECK(flushes==0 && handles==0);
    CHECK(GuardRecordWaveSnapshot(&h)==STATUS_UNSUCCESSFUL && handles==0);
    printf("Wave recovery persistence: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
