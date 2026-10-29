// Production protected-file adapter with an in-memory Windows file API. Real
// security-descriptor parsing checks the exact owner and ACEs, without touching
// a host file, registry key or token. Each storage failure remains distinguishable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <bcrypt.h>
#include <stdio.h>
#include "windows_migration.h"
#include "windows_handoff.h"
#include "windows_lock.h"
#include "installer_rollback.h"
#include "../kmd/board_memory_format.h"
typedef struct fake_file {WCHAR name[200];BYTE *data;DWORD bytes,capacity;BOOL exists,dir,reparse;UINT security;} FAKE_FILE;
typedef struct fake_handle {FAKE_FILE *file;DWORD offset;BOOL token,remove;} FAKE_HANDLE;
static FAKE_FILE Files[64];static UINT Count,Handles,LockCount,IoCount,FailAt,Checks,Failures;
static DWORD FakeError;static BYTE Active[16];static BOOL StaleAfterRead;static UINT IdentityReads;
static BOARD_STORE_RECORD LegacyBoard;static BOOL LegacyPresent;static DWORD RegistryFault;
static DWORD WINAPI LastError(void){return FakeError;}
static BOOL Fault(void){IoCount++;if(FailAt==IoCount){FakeError=ERROR_WRITE_FAULT;return TRUE;}return FALSE;}
static FAKE_FILE *Find(PCWSTR path)
{UINT i;for(i=0;i<Count;i++)if(Files[i].exists && !wcscmp(Files[i].name,path))return &Files[i];return NULL;}
static FAKE_FILE *Make(PCWSTR path,BOOL directory)
{
    FAKE_FILE *f;if(Count==64)return NULL;f=&Files[Count++];ZeroMemory(f,sizeof(*f));
    (void)StringCchCopyW(f->name,200,path);f->exists=TRUE;f->dir=directory;return f;
}
static HANDLE Handle(FAKE_FILE *file)
{FAKE_HANDLE *h=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*h));if(!h)return INVALID_HANDLE_VALUE;h->file=file;Handles++;return h;}
static BOOL WINAPI Close(HANDLE handle)
{
    FAKE_HANDLE *h=handle;if(!h || handle==INVALID_HANDLE_VALUE){FakeError=ERROR_INVALID_HANDLE;return FALSE;}
    if(h->remove && h->file)h->file->exists=FALSE;HeapFree(GetProcessHeap(),0,h);Handles--;return TRUE;
}
static BOOL WINAPI OpenToken(HANDLE process,DWORD rights,PHANDLE token)
{(void)process;(void)rights;*token=Handle(NULL);((FAKE_HANDLE*)*token)->token=TRUE;return TRUE;}
static BOOL WINAPI Token(HANDLE token,TOKEN_INFORMATION_CLASS kind,LPVOID data,DWORD size,PDWORD needed)
{
    DWORD sid_bytes=SECURITY_MAX_SID_SIZE;TOKEN_USER *user=data;(void)token;(void)kind;
    *needed=sizeof(*user)+sid_bytes;if(size<*needed)return FALSE;
    user->User.Sid=(BYTE*)data+sizeof(*user);user->User.Attributes=0;
    return CreateWellKnownSid(WinLocalSystemSid,NULL,user->User.Sid,&sid_bytes);
}
static DWORD WINAPI Environment(LPCWSTR name,LPWSTR out,DWORD capacity)
{(void)name;if(FAILED(StringCchCopyW(out,capacity,L"fixture")))return 0;return 7;}
static BOOL WINAPI Mkdir(LPCWSTR name,LPSECURITY_ATTRIBUTES sa)
{(void)sa;if(Fault())return FALSE;if(Find(name)){FakeError=ERROR_ALREADY_EXISTS;return FALSE;}return Make(name,TRUE)!=NULL;}
static HANDLE WINAPI Open(LPCWSTR name,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES sa,DWORD disposition,DWORD flags,HANDLE template_file)
{
    FAKE_FILE *f;(void)access;(void)sharing;(void)sa;(void)flags;(void)template_file;
    if(Fault())return INVALID_HANDLE_VALUE;f=Find(name);
    if(disposition==CREATE_NEW){if(f){FakeError=ERROR_FILE_EXISTS;return INVALID_HANDLE_VALUE;}f=Make(name,FALSE);}
    if(!f){FakeError=ERROR_FILE_NOT_FOUND;return INVALID_HANDLE_VALUE;}return Handle(f);
}
static BOOL WINAPI Info(HANDLE handle,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID buffer,DWORD bytes)
{
    FAKE_FILE *f=((FAKE_HANDLE*)handle)->file;FILE_ATTRIBUTE_TAG_INFO *tag=buffer;(void)kind;(void)bytes;
    tag->FileAttributes=(f->dir?FILE_ATTRIBUTE_DIRECTORY:0)|(f->reparse?FILE_ATTRIBUTE_REPARSE_POINT:0);tag->ReparseTag=0;return TRUE;
}
static DWORD WINAPI Security(HANDLE handle,SE_OBJECT_TYPE type,SECURITY_INFORMATION info,PSID *owner,PSID *group,PACL *acl,PACL *sacl,PSECURITY_DESCRIPTOR *sd)
{
    FAKE_FILE *f=((FAKE_HANDLE*)handle)->file;BOOL present,defaulted;PCWSTR text=L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FR;;;BA)";
    (void)info;(void)group;(void)sacl;
    if(type==SE_REGISTRY_KEY){f=NULL;text=L"O:SYG:SYD:P(A;;KA;;;SY)(A;;KR;;;BA)";}
    if(f) {
    if(f->security==1)text=L"O:BAG:SYD:P(A;;FA;;;SY)(A;;FR;;;BA)";
    if(f->security==2)text=L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)";
    if(f->security==3)text=L"O:SYG:SYD:(A;;FA;;;SY)(A;;FR;;;BA)";
    if(f->security==4)return ERROR_ACCESS_DENIED;
    }
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,sd,NULL))return ERROR_INVALID_SECURITY_DESCR;
    GetSecurityDescriptorOwner(*sd,owner,&defaulted);GetSecurityDescriptorDacl(*sd,&present,acl,&defaulted);return 0;
}
static BOOL WINAPI Size(HANDLE handle,PLARGE_INTEGER out)
{out->QuadPart=((FAKE_HANDLE*)handle)->file->bytes;return TRUE;}
static BOOL WINAPI Read(HANDLE handle,LPVOID data,DWORD bytes,LPDWORD got,LPOVERLAPPED overlap)
{
    FAKE_HANDLE *h=handle;DWORD left=h->file->bytes-h->offset;(void)overlap;if(Fault())return FALSE;
    *got=bytes<left?bytes:left;if(*got)memcpy(data,h->file->data+h->offset,*got);h->offset+=*got;return TRUE;
}
static BOOL WINAPI Write(HANDLE handle,LPCVOID data,DWORD bytes,LPDWORD done,LPOVERLAPPED overlap)
{
    FAKE_HANDLE *h=handle;FAKE_FILE *f=h->file;(void)overlap;if(Fault())return FALSE;
    if(h->offset+bytes>f->capacity) {
        BYTE *next=HeapAlloc(GetProcessHeap(),0,h->offset+bytes);if(!next)return FALSE;
        if(f->bytes)memcpy(next,f->data,f->bytes);if(f->data)HeapFree(GetProcessHeap(),0,f->data);
        f->data=next;f->capacity=h->offset+bytes;
    }
    memcpy(f->data+h->offset,data,bytes);h->offset+=bytes;if(f->bytes<h->offset)f->bytes=h->offset;*done=bytes;return TRUE;
}
static BOOL WINAPI Flush(HANDLE handle){(void)handle;return !Fault();}
static BOOL WINAPI Seek(HANDLE handle,LARGE_INTEGER position,PLARGE_INTEGER actual,DWORD method)
{(void)actual;(void)method;if(Fault())return FALSE;((FAKE_HANDLE*)handle)->offset=(DWORD)position.QuadPart;return TRUE;}
static BOOL WINAPI Move(LPCWSTR from,LPCWSTR to,DWORD flags)
{
    FAKE_FILE *f;(void)flags;if(Fault())return FALSE;
    f=Find(from);if(!f){FakeError=ERROR_FILE_NOT_FOUND;return FALSE;}if(Find(to)){FakeError=ERROR_ALREADY_EXISTS;return FALSE;}
    (void)StringCchCopyW(f->name,200,to);return TRUE;
}
static BOOL WINAPI Dispose(HANDLE handle,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD bytes)
{(void)kind;(void)bytes;if(Fault())return FALSE;((FAKE_HANDLE*)handle)->remove=((FILE_DISPOSITION_INFO*)data)->DeleteFile;return TRUE;}
static LSTATUS WINAPI RegCreate(HKEY root,LPCWSTR path,DWORD reserved,LPWSTR cls,DWORD options,REGSAM access,
    const LPSECURITY_ATTRIBUTES sa,PHKEY out,LPDWORD disposition)
{(void)root;(void)path;(void)reserved;(void)cls;(void)options;(void)access;(void)sa;
 if(RegistryFault || Fault())return ERROR_ACCESS_DENIED;*out=(HKEY)Handle(NULL);*disposition=LegacyPresent?REG_OPENED_EXISTING_KEY:REG_CREATED_NEW_KEY;return 0;}
static LSTATUS WINAPI RegRead(HKEY key,LPCWSTR name,LPDWORD reserved,LPDWORD type,LPBYTE data,LPDWORD bytes)
{(void)key;(void)name;(void)reserved;if(Fault())return ERROR_READ_FAULT;if(!LegacyPresent)return ERROR_FILE_NOT_FOUND;
 if(*bytes<sizeof(LegacyBoard))return ERROR_MORE_DATA;*type=REG_BINARY;*bytes=sizeof(LegacyBoard);memcpy(data,&LegacyBoard,sizeof(LegacyBoard));return 0;}
static LSTATUS WINAPI RegWrite(HKEY key,LPCWSTR name,DWORD reserved,DWORD type,const BYTE *data,DWORD bytes)
{(void)key;(void)name;(void)reserved;(void)type;if(Fault())return ERROR_WRITE_FAULT;if(bytes!=sizeof(LegacyBoard))return ERROR_INVALID_DATA;
 memcpy(&LegacyBoard,data,bytes);LegacyPresent=TRUE;return 0;}
static LSTATUS WINAPI RegFlush(HKEY key){(void)key;return Fault()?ERROR_WRITE_FAULT:0;}
static LSTATUS WINAPI RegClose(HKEY key){return Close(key)?0:ERROR_INVALID_HANDLE;}
#define GetLastError LastError
#define OpenProcessToken OpenToken
#define GetTokenInformation Token
#define GetEnvironmentVariableW Environment
#define CreateDirectoryW Mkdir
#define CreateFileW Open
#define CloseHandle Close
#define GetFileInformationByHandleEx Info
#define GetSecurityInfo Security
#define GetFileSizeEx Size
#define ReadFile Read
#define WriteFile Write
#define FlushFileBuffers Flush
#define SetFilePointerEx Seek
#define MoveFileExW Move
#define SetFileInformationByHandle Dispose
#define RegCreateKeyExW RegCreate
#define RegQueryValueExW RegRead
#define RegSetValueExW RegWrite
#define RegFlushKey RegFlush
#define RegCloseKey RegClose
#include "windows_handoff.c"
#undef GetLastError
DWORD AmdgpuStateWriterLock(HANDLE *lock){LockCount++;*lock=(HANDLE)1;return 0;}
void AmdgpuStateWriterUnlock(HANDLE lock){if(lock)LockCount--;}
UINT AmdgpuWindowsStateOpen(AMDGPU_WINDOWS_STATE **out,UINT writable,DWORD *error)
{(void)writable;*error=0;*out=(AMDGPU_WINDOWS_STATE*)1;return 0;}
void AmdgpuWindowsStateClose(AMDGPU_WINDOWS_STATE *state){(void)state;}
UINT AmdgpuWindowsStateIdentity(AMDGPU_WINDOWS_STATE *state,BYTE generation[16],DWORD *error)
{(void)state;IdentityReads++;memcpy(generation,Active,16);if(StaleAfterRead && IdentityReads>1)generation[0]++;*error=0;return 0;}
#define CHECK(x) do{Checks++;if(!(x)){printf("FAIL %u line %u: %s\n",Checks,__LINE__,#x);Failures++;}}while(0)
static void Reset(void)
{
    UINT i;CHECK(!Handles && !LockCount);
    for(i=0;i<Count;i++)if(Files[i].data)HeapFree(GetProcessHeap(),0,Files[i].data);
    ZeroMemory(Files,sizeof(Files));Count=IoCount=FailAt=0;IdentityReads=0;StaleAfterRead=FALSE;
    memset(Active,1,16);Make(L"fixture",TRUE);
    LegacyPresent=FALSE;RegistryFault=0;ZeroMemory(&LegacyBoard,sizeof(LegacyBoard));
}
static void Put(PCWSTR name,const void *data,DWORD bytes)
{FAKE_FILE *f=Make(name,FALSE);f->data=HeapAlloc(GetProcessHeap(),0,bytes);memcpy(f->data,data,bytes);f->bytes=f->capacity=bytes;}
static void BoardFixture(BOOL pending)
{
    BOARD_STORE_RECORD original={0};BOARD_STORE_JOURNAL journal={0};
    Make(L"fixture\\amdgpu-wddm",TRUE);Make(L"fixture\\amdgpu-wddm\\BoardMemory",TRUE);
    original.Magic=BOARD_STORE_MAGIC;original.Version=BOARD_STORE_VERSION;original.Bytes=sizeof(original);
    original.Mapping=1;original.BiosId=2;memset(original.MachineId,7,16);memset(original.Backup,3,28);original.Checksum=StoreChecksum(&original);
    journal.Magic=BOARD_JOURNAL_MAGIC;journal.Version=BOARD_JOURNAL_VERSION;journal.Bytes=sizeof(journal);
    journal.Sequence=5;journal.State=pending?BOARD_JOURNAL_PENDING:BOARD_JOURNAL_CLEAR;
    journal.Mapping=original.Mapping;journal.BiosId=original.BiosId;memcpy(journal.MachineId,original.MachineId,16);
    journal.OriginalChecksum=original.Checksum;journal.Checksum=JournalChecksum(&journal);
    Put(L"fixture\\amdgpu-wddm\\BoardMemory\\original.dat",&original,sizeof(original));
    Put(L"fixture\\amdgpu-wddm\\BoardMemory\\journal.dat",&journal,sizeof(journal));
}
static UINT Empty(void *context,UINT source,const AMDGPU_STATE_SPEC *spec,AMDGPU_STATE_VALUE *value,void *data,UINT capacity)
{(void)context;(void)source;(void)spec;(void)value;(void)data;(void)capacity;return AMDGPU_STATE_ABSENT;}
// The same frame as another release writes it under its own state catalog: one changed catalog character and a
// valid checksum (FNV-1a over the record, without the checksum field). Two calls give the original record again.
static void OtherCatalog(BYTE *p,UINT bytes)
{
    AMDGPU_TRANSFER_HEADER h;UINT i,sum=2166136261u,at=offsetof(AMDGPU_TRANSFER_HEADER,checksum);
    p[offsetof(AMDGPU_TRANSFER_HEADER,catalog)]^=1;
    for(i=0;i<bytes;i++){sum^=(i>=at && i<at+4)?0:p[i];sum*=16777619u;}
    memcpy(&h,p,sizeof(h));h.checksum=sum;memcpy(p,&h,sizeof(h));
}
// The named fake file exists and holds exactly these bytes.
static BOOL Holds(PCWSTR name,const void *data,DWORD bytes)
{FAKE_FILE *f=Find(name);return f && f->bytes==bytes && !memcmp(f->data,data,bytes);}
static void RemovalRecords(const BYTE *snapshot,UINT bytes)
{
    static const WCHAR old[]=L"fixture\\amdgpu-wddm\\StateV2\\removed.bin",cur[]=L"fixture\\amdgpu-wddm\\StateV2\\removed-v2.bin",
        old_retired[]=L"fixture\\amdgpu-wddm\\StateV2\\retired-01010101010101010101010101010101-removed.bin",
        cur_retired[]=L"fixture\\amdgpu-wddm\\StateV2\\retired-01010101010101010101010101010101-removed-v2.bin";
    BYTE tx[16],*other=HeapAlloc(GetProcessHeap(),0,bytes),*record=HeapAlloc(GetProcessHeap(),0,AMDGPU_TRANSFER_MAX_BYTES);
    BYTE *pending=HeapAlloc(GetProcessHeap(),0,AMDGPU_TRANSFER_MAX_BYTES);
    AMDGPU_TRANSFER_HEADER h;void *read=NULL;DWORD got=0;UINT record_bytes=0,pending_bytes=0;
    memset(tx,1,16);memcpy(other,snapshot,bytes);
    OtherCatalog(other,bytes);CHECK(AmdgpuTransferCheck(other,bytes,NULL)!=0);
    OtherCatalog(other,bytes);CHECK(!memcmp(other,snapshot,bytes));OtherCatalog(other,bytes);
    // The uninstall writes removed-v2.bin. A release up to 0.7.216.105 reads only removed.bin, and it stops on a
    // record of another catalog (the check above): after this uninstall it finds no record and installs fresh.
    Reset();CHECK(!AmdgpuWindowsArchiveState(snapshot,bytes));CHECK(Find(cur)!=NULL && !Find(old));
    CHECK(!AmdgpuWindowsReadRemoval(&read,&got,TRUE));CHECK(got==bytes && !memcmp(read,snapshot,bytes));HeapFree(GetProcessHeap(),0,read);
    // The removal record of another release under either name: its intact bytes go to the worker, which maps or
    // retires it; retire moves it aside as evidence.
    Reset();Put(old,other,bytes);read=NULL;got=0;
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,FALSE)==ERROR_UNSUPPORTED_TYPE);CHECK(got==bytes && read && !memcmp(read,other,bytes));
    HeapFree(GetProcessHeap(),0,read);read=NULL;
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,TRUE)==ERROR_FILE_NOT_FOUND);
    CHECK(!AmdgpuWindowsRetireRemoval(tx,FALSE));CHECK(!Find(old) && Find(old_retired)!=NULL);
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,FALSE)==ERROR_FILE_NOT_FOUND);
    CHECK(!AmdgpuWindowsRetireRemoval(tx,FALSE));CHECK(Holds(old_retired,other,bytes));CHECK(!Handles && !LockCount);
    Reset();Put(cur,other,bytes);
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,FALSE)==ERROR_UNSUPPORTED_TYPE);HeapFree(GetProcessHeap(),0,read);read=NULL;
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,TRUE)==ERROR_UNSUPPORTED_TYPE);HeapFree(GetProcessHeap(),0,read);read=NULL;
    CHECK(!AmdgpuWindowsRetireRemoval(tx,FALSE));CHECK(!Find(cur) && Find(cur_retired)!=NULL);
    // Evidence is never overwritten: an existing retired name stops the move.
    Reset();Put(old,other,bytes);Put(old_retired,other,bytes);
    CHECK(AmdgpuWindowsRetireRemoval(tx,FALSE)==ERROR_ALREADY_EXISTS);CHECK(Find(old)!=NULL);
    // Damage is not another catalog: a wrong magic or a wrong checksum stays an error, under either catalog.
    Reset();Put(old,other,bytes);Find(old)->data[0]^=1;
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,FALSE)==ERROR_INVALID_DATA);CHECK(!read);
    Reset();Put(old,other,bytes);Find(old)->data[bytes-1]^=1;
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,FALSE)==ERROR_INVALID_DATA);CHECK(!read);
    Reset();Put(old,snapshot,bytes);Find(old)->data[bytes-1]^=1;
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,FALSE)==ERROR_INVALID_DATA);
    // removed.bin beside removed-v2.bin is the newer record. Keep moves only the older one.
    Reset();Put(old,snapshot,bytes);Put(cur,other,bytes);
    CHECK(!AmdgpuWindowsReadRemoval(&read,&got,FALSE));CHECK(got==bytes && !memcmp(read,snapshot,bytes));HeapFree(GetProcessHeap(),0,read);
    CHECK(AmdgpuWindowsReadRemoval(&read,&got,TRUE)==ERROR_UNSUPPORTED_TYPE);HeapFree(GetProcessHeap(),0,read);read=NULL;
    CHECK(!AmdgpuWindowsRetireRemoval(tx,TRUE));CHECK(Find(old)!=NULL && !Find(cur) && Find(cur_retired)!=NULL);
    Reset();Put(cur,snapshot,bytes);CHECK(!AmdgpuWindowsRetireRemoval(tx,TRUE));CHECK(Find(cur)!=NULL);
    // The acknowledgment of the reinstall removes the record that the prepare imported, under either name.
    memset(&h,0,sizeof(h));h.origin=AMDGPU_TRANSFER_CURRENT;h.source_schema=AmdgpuStateSchema();h.board_kind=AMDGPU_BOARD_UNPROVEN;
    memset(h.transaction,9,16);memset(h.source_generation,2,16);memset(h.device_hash,7,32);AmdgpuStateBootToken(90,h.source_boot);
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);strcpy_s(h.source_version,32,"0.7.216.100");strcpy_s(h.destination_version,32,"0.7.216.100");
    CHECK(!AmdgpuTransferCapture(&h,Empty,NULL,record,AMDGPU_TRANSFER_MAX_BYTES,&record_bytes));
    h.origin=AMDGPU_TRANSFER_ARCHIVE;memcpy(h.transaction,tx,16);strcpy_s(h.destination_version,32,"0.7.216.102");AmdgpuStateBootToken(91,h.source_boot);
    CHECK(!AmdgpuTransferCapture(&h,Empty,NULL,pending,AMDGPU_TRANSFER_MAX_BYTES,&pending_bytes));
    Reset();Put(cur,record,record_bytes);CHECK(!AmdgpuWindowsPublishHandoff(pending,pending_bytes));
    CHECK(!AmdgpuWindowsAcknowledgeHandoff(tx,h.device_hash,h.destination_version));CHECK(!Find(cur));
    Reset();Put(old,record,record_bytes);CHECK(!AmdgpuWindowsPublishHandoff(pending,pending_bytes));
    CHECK(!AmdgpuWindowsAcknowledgeHandoff(tx,h.device_hash,h.destination_version));CHECK(!Find(old));
    // The prepare read a record of an earlier catalog by setting name: the acknowledgment removes that record too.
    OtherCatalog(record,record_bytes);
    Reset();Put(old,record,record_bytes);CHECK(!AmdgpuWindowsPublishHandoff(pending,pending_bytes));
    CHECK(!AmdgpuWindowsAcknowledgeHandoff(tx,h.device_hash,h.destination_version));CHECK(!Find(old));
    OtherCatalog(record,record_bytes);
    // An uninstall after an install that did not read the earlier record (an older release ignores removed-v2.bin):
    // the earlier records move aside, the new record is written, and a retry of the same uninstall keeps it.
    Reset();Put(cur,record,record_bytes);Put(old,other,bytes);
    CHECK(!AmdgpuWindowsArchiveState(snapshot,bytes));CHECK(!Find(old) && Find(old_retired)!=NULL);
    CHECK(Holds(cur,snapshot,bytes));CHECK(Holds(cur_retired,record,record_bytes));
    CHECK(!AmdgpuWindowsArchiveState(snapshot,bytes));CHECK(Find(cur)!=NULL && !Find(old));
    Reset();Put(cur,snapshot,bytes);Find(cur)->data[bytes-1]^=1;
    CHECK(AmdgpuWindowsArchiveState(snapshot,bytes)==ERROR_INVALID_DATA);CHECK(!Find(cur_retired));
    CHECK(!Handles && !LockCount);
    HeapFree(GetProcessHeap(),0,other);HeapFree(GetProcessHeap(),0,record);HeapFree(GetProcessHeap(),0,pending);
}
int main(void)
{
    BYTE *snapshot=HeapAlloc(GetProcessHeap(),0,AMDGPU_TRANSFER_MAX_BYTES);AMDGPU_TRANSFER_HEADER h={0};UINT bytes=0,total,i;
    void *read=NULL;DWORD got,error;FAKE_FILE *f;
    h.origin=AMDGPU_TRANSFER_FRESH;h.board_kind=AMDGPU_BOARD_UNPROVEN;memset(h.transaction,1,16);memset(h.device_hash,7,32);
    AmdgpuStateBootToken(91,h.source_boot);memcpy(h.catalog,AmdgpuStateCatalogId(),64);strcpy_s(h.destination_version,32,"0.7.216.102");
    CHECK(!AmdgpuTransferCapture(&h,Empty,NULL,snapshot,AMDGPU_TRANSFER_MAX_BYTES,&bytes));
    Reset();CHECK(!AmdgpuWindowsPublishHandoff(snapshot,bytes));total=IoCount;CHECK(!Handles && !LockCount);
    CHECK(!AmdgpuWindowsReadHandoff(&read,&got));CHECK(got==bytes && !memcmp(read,snapshot,bytes));HeapFree(GetProcessHeap(),0,read);
    CHECK(!AmdgpuWindowsPublishHandoff(snapshot,bytes));
    Active[0]=2;CHECK(AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version)==ERROR_REVISION_MISMATCH);
    CHECK(Find(L"fixture\\amdgpu-wddm\\StateV2\\pending.bin")!=NULL);Active[0]=1;
    StaleAfterRead=TRUE;IdentityReads=0;
    CHECK(AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version)==ERROR_REVISION_MISMATCH);
    CHECK(Find(L"fixture\\amdgpu-wddm\\StateV2\\pending.bin")!=NULL);StaleAfterRead=FALSE;
    CHECK(!AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version));
    CHECK(!Find(L"fixture\\amdgpu-wddm\\StateV2\\pending.bin"));
    CHECK(!AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version));
    Active[0]=2;CHECK(AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version)==ERROR_REVISION_MISMATCH);
    // Every publication IO fault is followed by the actual production reader.
    // A partially written temporary file can never be consumed as pending.
    for(i=1;i<=total;i++) {
        Reset();FailAt=i;error=AmdgpuWindowsPublishHandoff(snapshot,bytes);CHECK(error!=0);FailAt=0;
        error=AmdgpuWindowsReadHandoff(&read,&got);
        CHECK(error==ERROR_FILE_NOT_FOUND || (!error && got==bytes && !memcmp(read,snapshot,bytes)));
        if(read)HeapFree(GetProcessHeap(),0,read);CHECK(!Handles && !LockCount);
    }
    for(i=1;i<=4;i++) {
        Reset();CHECK(!AmdgpuWindowsPublishHandoff(snapshot,bytes));
        f=Find(L"fixture\\amdgpu-wddm\\StateV2\\pending.bin");f->security=i;
        CHECK(AmdgpuWindowsReadHandoff(&read,&got)!=0);CHECK(!read && !got);
        CHECK(AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version)!=0);
        CHECK(f->exists);
    }
    Reset();CHECK(!AmdgpuWindowsPublishHandoff(snapshot,bytes));
    f=Find(L"fixture\\amdgpu-wddm\\StateV2");f->reparse=TRUE;
    CHECK(AmdgpuWindowsReadHandoff(&read,&got)==ERROR_REPARSE_TAG_INVALID);CHECK(!read && !got);
    Reset();CHECK(!AmdgpuWindowsPublishHandoff(snapshot,bytes));
    f=Find(L"fixture\\amdgpu-wddm\\StateV2\\pending.bin");f->data[0]^=1;
    CHECK(AmdgpuWindowsReadHandoff(&read,&got)==ERROR_INVALID_DATA);
    CHECK(AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version)==ERROR_INVALID_DATA);CHECK(f->exists);
    Reset();CHECK(!AmdgpuWindowsArchiveState(snapshot,bytes));
    CHECK(!AmdgpuWindowsReadRemoval(&read,&got,FALSE));CHECK(got==bytes && !memcmp(read,snapshot,bytes));HeapFree(GetProcessHeap(),0,read);
    CHECK(!AmdgpuWindowsArchiveState(snapshot,bytes));
    Reset();CHECK(!AmdgpuWindowsArchiveState(snapshot,bytes));total=IoCount;
    for(i=1;i<=total;i++) {
        Reset();FailAt=i;error=AmdgpuWindowsArchiveState(snapshot,bytes);CHECK(error!=0);FailAt=0;
        error=AmdgpuWindowsReadRemoval(&read,&got,FALSE);
        CHECK(error==ERROR_FILE_NOT_FOUND || error==ERROR_INVALID_DATA ||
              (!error && got==bytes && !memcmp(read,snapshot,bytes)));
        if(read)HeapFree(GetProcessHeap(),0,read);CHECK(!Handles && !LockCount);
    }
    RemovalRecords(snapshot,bytes);
    Reset();CHECK(!AmdgpuWindowsPublishHandoff(snapshot,bytes));IoCount=0;
    CHECK(!AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version));total=IoCount;
    for(i=1;i<=total;i++) {
        Reset();CHECK(!AmdgpuWindowsPublishHandoff(snapshot,bytes));IoCount=0;FailAt=i;
        error=AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version);CHECK(error!=0);FailAt=0;
        CHECK(Find(L"fixture\\amdgpu-wddm\\StateV2\\pending.bin")!=NULL);
        CHECK(!Handles && !LockCount);
    }
    {AMDGPU_ROLLBACK *r=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,AMDGPU_ROLLBACK_MAX);
     h.origin=AMDGPU_TRANSFER_CURRENT;h.source_schema=AmdgpuStateSchema();memset(h.source_generation,2,16);strcpy_s(h.source_version,32,"0.7.216.103");
     CHECK(!AmdgpuTransferCapture(&h,Empty,NULL,&r->snapshot,AMDGPU_TRANSFER_MAX_BYTES,&bytes));
     r->magic=AMDGPU_ROLLBACK_MAGIC;r->version=1;r->bytes=AMDGPU_ROLLBACK_PREFIX+bytes;r->snapshot_bytes=bytes;
     memset(r->inf_hash,1,96);wcscpy_s(r->source_inf,32,L"oem17.inf");memset(r->source_inf_hash,4,32);
     Reset();CHECK(!AmdgpuWindowsRollbackSave(r,r->bytes,2));total=IoCount;
     CHECK(!AmdgpuWindowsRollbackRead(h.transaction,2,&read,&got));CHECK(got==r->bytes && !memcmp(read,r,got));HeapFree(GetProcessHeap(),0,read);
     CHECK(!AmdgpuWindowsRollbackSave(r,r->bytes,FALSE));CHECK(!AmdgpuWindowsRollbackSave(r,r->bytes,TRUE));
     r->sys_hash[0]^=1;CHECK(AmdgpuWindowsRollbackClear(r,r->bytes)==ERROR_REVISION_MISMATCH);r->sys_hash[0]^=1;
     CHECK(!AmdgpuWindowsRollbackClear(r,r->bytes));CHECK(!Find(L"fixture\\amdgpu-wddm\\StateV2\\rollback-active.bin"));
     for(i=1;i<=total;i++) {
         Reset();FailAt=i;error=AmdgpuWindowsRollbackSave(r,r->bytes,2);CHECK(error!=0);FailAt=0;
         error=AmdgpuWindowsRollbackRead(h.transaction,2,&read,&got);
         CHECK(error==ERROR_FILE_NOT_FOUND || error==ERROR_INVALID_DATA || (!error && got==r->bytes && !memcmp(read,r,got)));
         if(read)HeapFree(GetProcessHeap(),0,read);CHECK(!Handles && !LockCount);
     }
     Reset();CHECK(!AmdgpuWindowsRollbackSave(r,r->bytes,2));f=Find(L"fixture\\amdgpu-wddm\\StateV2\\rollback-active.bin");f->security=2;
     CHECK(AmdgpuWindowsRollbackRead(h.transaction,2,&read,&got)==ERROR_INVALID_SECURITY_DESCR);
     CHECK(AmdgpuWindowsRollbackClear(r,r->bytes)==ERROR_INVALID_SECURITY_DESCR);CHECK(f->exists);
     HeapFree(GetProcessHeap(),0,r);}
    Reset();BoardFixture(FALSE);CHECK(!AmdgpuWindowsRollbackBoard());CHECK(LegacyPresent && LegacyBoard.Pending==0);total=IoCount;
    CHECK(!AmdgpuWindowsRollbackBoard());LegacyBoard.Backup[0]^=1;LegacyBoard.Checksum=StoreChecksum(&LegacyBoard);
    CHECK(AmdgpuWindowsRollbackBoard()==ERROR_REVISION_MISMATCH);
    Reset();BoardFixture(TRUE);CHECK(AmdgpuWindowsRollbackBoard()==ERROR_INVALID_DATA);CHECK(!LegacyPresent);
    Reset();BoardFixture(FALSE);RegistryFault=1;CHECK(AmdgpuWindowsRollbackBoard()==ERROR_ACCESS_DENIED);CHECK(!LegacyPresent);
    for(i=1;i<=total;i++){Reset();BoardFixture(FALSE);FailAt=i;CHECK(AmdgpuWindowsRollbackBoard()!=0);CHECK(!Handles && !LockCount);}
    Reset();HeapFree(GetProcessHeap(),0,snapshot);
    printf("installer store: %u checks, %u failures\n",Checks,Failures);return Failures?1:0;
}
