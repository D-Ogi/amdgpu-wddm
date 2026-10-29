#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <bcrypt.h>
#include "windows_handoff.h"
#include "windows_lock.h"
#include "transfer.h"
#include "windows_migration.h"
#include "installer_rollback.h"
#include "installer_history.h"
#include "../kmd/board_memory_format.h"
static DWORD SystemIdentity(void)
{
    HANDLE token=NULL;BYTE user[256],sid[SECURITY_MAX_SID_SIZE];DWORD bytes=0,size=sizeof(sid),error=0;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return GetLastError();
    if(!GetTokenInformation(token,TokenUser,user,sizeof(user),&bytes) ||
       !CreateWellKnownSid(WinLocalSystemSid,NULL,sid,&size))error=GetLastError();
    else if(!EqualSid(((TOKEN_USER*)user)->User.Sid,sid))error=ERROR_ACCESS_DENIED;
    CloseHandle(token);return error;
}
static DWORD Secure(HANDLE file,BOOL directory)
{
    FILE_ATTRIBUTE_TAG_INFO tag;PSECURITY_DESCRIPTOR sd=NULL;PSID owner=NULL;PACL acl=NULL;
    BYTE sys[SECURITY_MAX_SID_SIZE],admin[SECURITY_MAX_SID_SIZE];DWORD sb=sizeof(sys),ab=sizeof(admin),error,i,revision;
    SECURITY_DESCRIPTOR_CONTROL control;
    if(!GetFileInformationByHandleEx(file,FileAttributeTagInfo,&tag,sizeof(tag)))return GetLastError();
    if(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT || !!(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=!!directory)
        return ERROR_REPARSE_TAG_INVALID;
    error=GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
                          &owner,NULL,&acl,NULL,&sd);if(error)return error;
    if(!CreateWellKnownSid(WinLocalSystemSid,NULL,sys,&sb) ||
       !CreateWellKnownSid(WinBuiltinAdministratorsSid,NULL,admin,&ab) ||
       !GetSecurityDescriptorControl(sd,&control,&revision) || !(control&SE_DACL_PROTECTED) ||
       !owner || !EqualSid(owner,sys) || !acl || acl->AceCount!=2) {error=ERROR_INVALID_SECURITY_DESCR;goto done;}
    for(i=0;i<2;i++) {
        ACCESS_ALLOWED_ACE *ace;PSID sid=i?admin:sys;DWORD mask=i?FILE_GENERIC_READ:FILE_ALL_ACCESS;
        if(!GetAce(acl,i,(void**)&ace) || ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags ||
           ace->Mask!=mask || ace->Header.AceSize!=FIELD_OFFSET(ACCESS_ALLOWED_ACE,SidStart)+GetLengthSid(sid) ||
           !EqualSid(&ace->SidStart,sid)) {error=ERROR_INVALID_SECURITY_DESCR;goto done;}
    }
done:
    LocalFree(sd);return error;
}
static DWORD OpenDirectory(WCHAR path[32768],PCWSTR leaf,SECURITY_ATTRIBUTES *sa,HANDLE *handle)
{
    DWORD error;
    if(FAILED(StringCchCatW(path,32768,leaf)))return ERROR_FILENAME_EXCED_RANGE;
    if(!CreateDirectoryW(path,sa) && GetLastError()!=ERROR_ALREADY_EXISTS)return GetLastError();
    *handle=CreateFileW(path,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(*handle==INVALID_HANDLE_VALUE){*handle=NULL;return GetLastError();}
    error=Secure(*handle,TRUE);return error;
}
static DWORD Matches(HANDLE file,const void *buffer,DWORD bytes)
{
    LARGE_INTEGER size;DWORD error=0,got=0;BYTE *read;
    error=Secure(file,FALSE);if(error)return error;
    if(!GetFileSizeEx(file,&size))return GetLastError();
    if(size.QuadPart!=bytes)return ERROR_REVISION_MISMATCH;
    read=HeapAlloc(GetProcessHeap(),0,bytes);if(!read)return ERROR_NOT_ENOUGH_MEMORY;
    if(!ReadFile(file,read,bytes,&got,NULL))error=GetLastError();
    else if(got!=bytes || memcmp(read,buffer,bytes))error=ERROR_REVISION_MISMATCH;
    HeapFree(GetProcessHeap(),0,read);return error;
}
DWORD AmdgpuWindowsPublishHandoff(const void *buffer,DWORD bytes)
{
    DWORD error,length,done=0;WCHAR *path=NULL,*temporary=NULL;HANDLE folders[3]={0},file=INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,FALSE};ULONG i;HANDLE writer=NULL;
    BYTE random[16];WCHAR suffix[47];static const WCHAR hex[]=L"0123456789abcdef";
    error=SystemIdentity();if(error)return error;
    if(AmdgpuTransferCheck(buffer,bytes,NULL)!=0)return ERROR_INVALID_DATA;
    error=AmdgpuStateWriterLock(&writer);if(error)return error;
    path=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,32768*sizeof(WCHAR));
    temporary=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,32768*sizeof(WCHAR));
    if(!path || !temporary){error=ERROR_NOT_ENOUGH_MEMORY;goto cleanup;}
    length=GetEnvironmentVariableW(L"DriverData",path,32768);
    if(!length || length>=32768){error=length?ERROR_FILENAME_EXCED_RANGE:GetLastError();goto cleanup;}
    folders[0]=CreateFileW(path,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(folders[0]==INVALID_HANDLE_VALUE){folders[0]=NULL;error=GetLastError();goto cleanup;}
    {
        FILE_ATTRIBUTE_TAG_INFO tag;
        if(!GetFileInformationByHandleEx(folders[0],FileAttributeTagInfo,&tag,sizeof(tag))){error=GetLastError();goto cleanup;}
        if(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT || !(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
            error=ERROR_REPARSE_TAG_INVALID;goto cleanup;
        }
    }
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FR;;;BA)",
        SDDL_REVISION_1,&sa.lpSecurityDescriptor,NULL)){error=GetLastError();goto cleanup;}
    error=OpenDirectory(path,L"\\amdgpu-wddm",&sa,&folders[1]);if(error)goto cleanup;
    error=OpenDirectory(path,L"\\StateV2",&sa,&folders[2]);if(error)goto cleanup;
    if(BCryptGenRandom(NULL,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){error=ERROR_GEN_FAILURE;goto cleanup;}
    (void)StringCchCopyW(suffix,47,L"\\pending-");
    for(i=0;i<16;i++){suffix[9+2*i]=hex[random[i]>>4];suffix[10+2*i]=hex[random[i]&15];}
    (void)StringCchCopyW(suffix+41,6,L".new");
    if(FAILED(StringCchCopyW(temporary,32768,path)) || FAILED(StringCchCatW(path,32768,L"\\pending.bin")) ||
       FAILED(StringCchCatW(temporary,32768,suffix))) {error=ERROR_FILENAME_EXCED_RANGE;goto cleanup;}
    file=CreateFileW(path,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file!=INVALID_HANDLE_VALUE){error=Matches(file,buffer,bytes);goto cleanup;}
    error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND)goto cleanup;
    file=CreateFileW(temporary,GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,&sa,CREATE_NEW,
                     FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE) {
        error=GetLastError();
        // An interrupted previous write is retained. Do not delete a file merely
        // because its name matches; the installer must report/resolve that state.
        goto cleanup;
    }
    error=Secure(file,FALSE);if(error)goto cleanup;
    if(!WriteFile(file,buffer,bytes,&done,NULL)){error=GetLastError();goto cleanup;}
    if(done!=bytes){error=ERROR_WRITE_FAULT;goto cleanup;}
    if(!FlushFileBuffers(file)){error=GetLastError();goto cleanup;}
    {
        LARGE_INTEGER zero={0};
        if(!SetFilePointerEx(file,zero,NULL,FILE_BEGIN)){error=GetLastError();goto cleanup;}
        error=Matches(file,buffer,bytes);if(error)goto cleanup;
    }
    if(!CloseHandle(file)){file=INVALID_HANDLE_VALUE;error=GetLastError();goto cleanup;}file=INVALID_HANDLE_VALUE;
    if(!MoveFileExW(temporary,path,MOVEFILE_WRITE_THROUGH)){error=GetLastError();goto cleanup;}
    error=ERROR_SUCCESS;
cleanup:
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    // Retain a failed temporary write for diagnosis. Never publish its partial
    // bytes and never remove a pre-existing pending file on an error path.
    for(i=3;i;i--)if(folders[i-1])CloseHandle(folders[i-1]);
    if(sa.lpSecurityDescriptor)LocalFree(sa.lpSecurityDescriptor);
    if(path)HeapFree(GetProcessHeap(),0,path);if(temporary)HeapFree(GetProcessHeap(),0,temporary);
    AmdgpuStateWriterUnlock(writer);
    return error;
}

typedef struct handoff_directory {
    WCHAR path[32768];
    HANDLE folders[3];
    SECURITY_ATTRIBUTES sa;
} HANDOFF_DIRECTORY;
static void CloseDirectory(HANDOFF_DIRECTORY *d)
{
    DWORD i;for(i=3;i;i--)if(d->folders[i-1])CloseHandle(d->folders[i-1]);
    if(d->sa.lpSecurityDescriptor)LocalFree(d->sa.lpSecurityDescriptor);
    HeapFree(GetProcessHeap(),0,d);
}
static DWORD Directory(HANDOFF_DIRECTORY **out)
{
    HANDOFF_DIRECTORY *d;DWORD error,length;FILE_ATTRIBUTE_TAG_INFO tag;
    *out=NULL;error=SystemIdentity();if(error)return error;
    d=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*d));if(!d)return ERROR_NOT_ENOUGH_MEMORY;
    d->sa.nLength=sizeof(d->sa);
    // SCM supplies the service environment. No request argument or interactive
    // process environment is copied to the SYSTEM worker.
    length=GetEnvironmentVariableW(L"DriverData",d->path,32768);
    if(!length || length>=32768){error=length?ERROR_FILENAME_EXCED_RANGE:GetLastError();goto fail;}
    d->folders[0]=CreateFileW(d->path,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(d->folders[0]==INVALID_HANDLE_VALUE){d->folders[0]=NULL;error=GetLastError();goto fail;}
    if(!GetFileInformationByHandleEx(d->folders[0],FileAttributeTagInfo,&tag,sizeof(tag))){error=GetLastError();goto fail;}
    if(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT || !(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
        error=ERROR_REPARSE_TAG_INVALID;goto fail;
    }
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FR;;;BA)",
        SDDL_REVISION_1,&d->sa.lpSecurityDescriptor,NULL)){error=GetLastError();goto fail;}
    error=OpenDirectory(d->path,L"\\amdgpu-wddm",&d->sa,&d->folders[1]);if(error)goto fail;
    error=OpenDirectory(d->path,L"\\StateV2",&d->sa,&d->folders[2]);if(error)goto fail;
    *out=d;return 0;
fail:
    CloseDirectory(d);return error;
}
static void TransactionLeaf(const BYTE transaction[16],WCHAR out[46])
{
    DWORD i;static const WCHAR hex[]=L"0123456789abcdef";
    (void)StringCchCopyW(out,46,L"\\archive-");
    for(i=0;i<16;i++){out[9+2*i]=hex[transaction[i]>>4];out[10+2*i]=hex[transaction[i]&15];}
    (void)StringCchCopyW(out+41,5,L".bin");
}
// With catalog set, an intact record of another state catalog gives ERROR_UNSUPPORTED_TYPE together with its bytes:
// it is a record of another release, not damage. The caller frees them.
static DWORD ReadLeaf(HANDOFF_DIRECTORY *d,PCWSTR leaf,void **out,DWORD *bytes,HANDLE *opened,BOOL catalog)
{
    HANDLE file;DWORD error,got=0;LARGE_INTEGER size;BYTE *data=NULL;
    *out=NULL;*bytes=0;
    if(FAILED(StringCchCatW(d->path,32768,leaf)))return ERROR_FILENAME_EXCED_RANGE;
    file=CreateFileW(d->path,GENERIC_READ|READ_CONTROL|(opened?DELETE:0),FILE_SHARE_READ,NULL,OPEN_EXISTING,
                     FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE)return GetLastError();
    error=Secure(file,FALSE);if(error)goto done;
    if(!GetFileSizeEx(file,&size)){error=GetLastError();goto done;}
    if(size.QuadPart<(LONGLONG)sizeof(AMDGPU_TRANSFER_HEADER) || size.QuadPart>AMDGPU_TRANSFER_MAX_BYTES){error=ERROR_INVALID_DATA;goto done;}
    data=HeapAlloc(GetProcessHeap(),0,(SIZE_T)size.QuadPart);if(!data){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
    if(!ReadFile(file,data,(DWORD)size.QuadPart,&got,NULL)){error=GetLastError();goto done;}
    if(got!=(DWORD)size.QuadPart){error=ERROR_INVALID_DATA;goto done;}
    if(AmdgpuTransferCheck(data,got,NULL)) {
        AMDGPU_TRANSFER_HEADER h;memcpy(&h,data,sizeof(h));
        if(!catalog || AmdgpuHistoryFrame(data,got) || !memcmp(h.catalog,AmdgpuStateCatalogId(),64)){error=ERROR_INVALID_DATA;goto done;}
        error=ERROR_UNSUPPORTED_TYPE;
    } else error=0;
    *out=data;*bytes=got;data=NULL;
    if(opened){*opened=file;file=INVALID_HANDLE_VALUE;}
done:
    if(data)HeapFree(GetProcessHeap(),0,data);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);return error;
}
DWORD AmdgpuWindowsReadHandoff(void **buffer,DWORD *bytes)
{
    HANDOFF_DIRECTORY *d=NULL;HANDLE lock=NULL;DWORD error;
    if(!buffer || !bytes)return ERROR_INVALID_PARAMETER;*buffer=NULL;*bytes=0;
    error=AmdgpuStateWriterLock(&lock);if(error)return error;
    error=Directory(&d);if(!error)error=ReadLeaf(d,L"\\pending.bin",buffer,bytes,NULL,FALSE);
    if(d)CloseDirectory(d);AmdgpuStateWriterUnlock(lock);return error;
}
// The removal record of an uninstall. The releases up to 0.7.216.105 read only removed.bin, and they stop on a
// record of another state catalog (BD-262). Thus this release writes removed-v2.bin, which those releases do not
// read, and an install goes back to such a release without a record. A removed.bin beside a removed-v2.bin is the
// newer record: only an older release writes removed.bin, and each install of this release moves it aside.
static const WCHAR RemovedOld[]=L"\\removed.bin",RemovedLeaf[]=L"\\removed-v2.bin";
DWORD AmdgpuWindowsReadRemoval(void **buffer,DWORD *bytes,BOOL own)
{
    HANDOFF_DIRECTORY *d=NULL;HANDLE lock=NULL;DWORD error;
    if(!buffer || !bytes)return ERROR_INVALID_PARAMETER;*buffer=NULL;*bytes=0;
    error=AmdgpuStateWriterLock(&lock);if(error)return error;
    error=ERROR_FILE_NOT_FOUND;
    if(!own){error=Directory(&d);if(!error)error=ReadLeaf(d,RemovedOld,buffer,bytes,NULL,TRUE);}
    if(error==ERROR_FILE_NOT_FOUND) {
        if(d)CloseDirectory(d);d=NULL;
        error=Directory(&d);if(!error)error=ReadLeaf(d,RemovedLeaf,buffer,bytes,NULL,TRUE);
    }
    if(d)CloseDirectory(d);AmdgpuStateWriterUnlock(lock);return error;
}
static DWORD RetireLeaf(const BYTE transaction[16],PCWSTR leaf)
{
    HANDOFF_DIRECTORY *d=NULL;WCHAR archive[46],*target;DWORD error;
    error=Directory(&d);if(error)return error;
    target=HeapAlloc(GetProcessHeap(),0,32768*sizeof(WCHAR));if(!target){CloseDirectory(d);return ERROR_NOT_ENOUGH_MEMORY;}
    TransactionLeaf(transaction,archive);
    if(FAILED(StringCchPrintfW(target,32768,L"%s\\retired-%.32s-%s",d->path,archive+9,leaf+1)) ||
       FAILED(StringCchCatW(d->path,32768,leaf)))error=ERROR_FILENAME_EXCED_RANGE;
    // No MOVEFILE_REPLACE_EXISTING: a retired record is evidence and is never overwritten.
    else if(!MoveFileExW(d->path,target,MOVEFILE_WRITE_THROUGH)){error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)error=0;}
    HeapFree(GetProcessHeap(),0,target);CloseDirectory(d);return error;
}
DWORD AmdgpuWindowsRetireRemoval(const unsigned char transaction[16],BOOL keep)
{
    HANDLE lock=NULL;DWORD error;BOOL old=FALSE;
    error=AmdgpuStateWriterLock(&lock);if(error)return error;
    if(keep) {
        // Keep the record that ReadRemoval returns. Only a removed-v2.bin beside a removed.bin is older.
        HANDOFF_DIRECTORY *d=NULL;HANDLE file;
        error=Directory(&d);
        if(!error && FAILED(StringCchCatW(d->path,32768,RemovedOld)))error=ERROR_FILENAME_EXCED_RANGE;
        if(!error) {
            file=CreateFileW(d->path,READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
            if(file!=INVALID_HANDLE_VALUE){old=TRUE;CloseHandle(file);}
            else{error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)error=0;}
        }
        if(d)CloseDirectory(d);
    } else error=RetireLeaf(transaction,RemovedOld);
    if(!error && (!keep || old))error=RetireLeaf(transaction,RemovedLeaf);
    AmdgpuStateWriterUnlock(lock);return error;
}
static DWORD Archive(HANDOFF_DIRECTORY *d,const void *buffer,DWORD bytes,PCWSTR fixed)
{
    AMDGPU_TRANSFER_HEADER h;WCHAR leaf[46];DWORD error,done=0;HANDLE file;
    if(AmdgpuTransferCheck(buffer,bytes,&h))return ERROR_INVALID_DATA;
    TransactionLeaf(h.transaction,leaf);
    if(FAILED(StringCchCatW(d->path,32768,fixed?fixed:leaf)))return ERROR_FILENAME_EXCED_RANGE;
    file=CreateFileW(d->path,GENERIC_READ|GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file!=INVALID_HANDLE_VALUE){
        error=Matches(file,buffer,bytes);
        // An earlier flush can have failed after writing every byte. A retry
        // must establish persistence, not mistake a cache read for a commit.
        if(!error && !FlushFileBuffers(file))error=GetLastError();
        if(!CloseHandle(file) && !error)error=GetLastError();return error;
    }
    error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND)return error;
    // CREATE_NEW never overwrites history. A torn archive remains an explicit
    // error on retry; it can never authorize service deletion or pending removal.
    file=CreateFileW(d->path,GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,&d->sa,CREATE_NEW,
                     FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE)return GetLastError();
    error=Secure(file,FALSE);
    if(!error && !WriteFile(file,buffer,bytes,&done,NULL))error=GetLastError();
    if(!error && done!=bytes)error=ERROR_WRITE_FAULT;
    if(!error && !FlushFileBuffers(file))error=GetLastError();
    if(!error){LARGE_INTEGER zero={0};if(!SetFilePointerEx(file,zero,NULL,FILE_BEGIN))error=GetLastError();}
    if(!error)error=Matches(file,buffer,bytes);
    if(!CloseHandle(file) && !error)error=GetLastError();return error;
}
// A removal record of another transaction, or of another state catalog, is older than this uninstall: no install
// read it (an install of an older release does not read removed-v2.bin). It moves aside before the new record.
// The record of this transaction stays: a resumed uninstall writes the same bytes again.
static DWORD RetireStale(const void *buffer,PCWSTR leaf)
{
    HANDOFF_DIRECTORY *d=NULL;AMDGPU_TRANSFER_HEADER h,old={0};void *data=NULL;DWORD got=0,error;
    memcpy(&h,buffer,sizeof(h));
    error=Directory(&d);if(!error)error=ReadLeaf(d,leaf,&data,&got,NULL,TRUE);
    if(d)CloseDirectory(d);
    if(data){memcpy(&old,data,sizeof(old));HeapFree(GetProcessHeap(),0,data);}
    if(!error && !memcmp(old.transaction,h.transaction,16))return 0;
    if(error==ERROR_FILE_NOT_FOUND)return 0;
    if(error && error!=ERROR_UNSUPPORTED_TYPE)return error;
    return RetireLeaf(h.transaction,leaf);
}
DWORD AmdgpuWindowsArchiveState(const void *buffer,DWORD bytes)
{
    HANDOFF_DIRECTORY *d=NULL;HANDLE lock=NULL;DWORD error;
    error=AmdgpuStateWriterLock(&lock);if(error)return error;
    error=Directory(&d);if(!error)error=Archive(d,buffer,bytes,NULL);
    if(d){CloseDirectory(d);d=NULL;}
    if(!error)error=RetireStale(buffer,RemovedOld);
    if(!error)error=RetireStale(buffer,RemovedLeaf);
    if(!error)error=Directory(&d);
    if(!error)error=Archive(d,buffer,bytes,RemovedLeaf);
    if(d)CloseDirectory(d);AmdgpuStateWriterUnlock(lock);return error;
}
DWORD AmdgpuWindowsAcknowledgeHandoff(const unsigned char transaction[16],
    const unsigned char device_hash[32],const char *product_version)
{
    HANDOFF_DIRECTORY *d=NULL,*archive=NULL;HANDLE lock=NULL,file=INVALID_HANDLE_VALUE;
    AMDGPU_WINDOWS_STATE *state=NULL;void *data=NULL;DWORD bytes=0,error,result;BYTE active[16];
    AMDGPU_TRANSFER_HEADER h;FILE_DISPOSITION_INFO dispose={TRUE};BOOL retained=FALSE;WCHAR leaf[46];
    error=AmdgpuStateWriterLock(&lock);if(error)return error;
    error=Directory(&d);if(error)goto done;
    error=ReadLeaf(d,L"\\pending.bin",&data,&bytes,&file,FALSE);
    if(error==ERROR_FILE_NOT_FOUND) {
        CloseDirectory(d);d=NULL;error=Directory(&d);if(error)goto done;
        TransactionLeaf(transaction,leaf);error=ReadLeaf(d,leaf,&data,&bytes,NULL,FALSE);retained=TRUE;
    }
    if(error)goto done;
    if(AmdgpuTransferCheck(data,bytes,&h) || memcmp(h.transaction,transaction,16) ||
       memcmp(h.device_hash,device_hash,32) || strcmp(h.destination_version,product_version)){
        error=ERROR_REVISION_MISMATCH;goto done;
    }
    result=AmdgpuWindowsStateOpen(&state,0,&error);if(result){if(!error)error=ERROR_INVALID_DATA;goto done;}
    result=AmdgpuWindowsStateIdentity(state,active,&error);
    if(result || memcmp(active,transaction,16)){if(!error)error=ERROR_REVISION_MISMATCH;goto done;}
    error=Directory(&archive);if(error)goto done;
    error=Archive(archive,data,bytes,NULL);if(error)goto done;
    result=AmdgpuWindowsStateIdentity(state,active,&error);
    if(result || memcmp(active,transaction,16)){if(!error)error=ERROR_REVISION_MISMATCH;goto done;}
    if(!retained && !SetFileInformationByHandle(file,FileDispositionInfo,&dispose,sizeof(dispose)))error=GetLastError();
    if(!error && h.origin==AMDGPU_TRANSFER_ARCHIVE) {
        HANDOFF_DIRECTORY *removed=NULL;void *old=NULL;DWORD old_bytes=0;HANDLE old_file=INVALID_HANDLE_VALUE;
        AMDGPU_TRANSFER_HEADER original;
        error=Directory(&removed);
        // The prepare of this transaction moved every other removal record aside: one of the two names remains.
        if(!error)error=ReadLeaf(removed,RemovedLeaf,&old,&old_bytes,&old_file,TRUE);
        if(error==ERROR_FILE_NOT_FOUND) {
            CloseDirectory(removed);removed=NULL;
            error=Directory(&removed);if(!error)error=ReadLeaf(removed,RemovedOld,&old,&old_bytes,&old_file,TRUE);
        }
        // The prepare reads a record of an earlier catalog by setting name; its intact frame names the same source.
        if(error==ERROR_UNSUPPORTED_TYPE)error=0;
        if(!error){memcpy(&original,old,sizeof(original));
            if(memcmp(original.source_generation,h.source_generation,16) || memcmp(original.device_hash,h.device_hash,32))
                error=ERROR_REVISION_MISMATCH;}
        if(!error && !SetFileInformationByHandle(old_file,FileDispositionInfo,&dispose,sizeof(dispose)))error=GetLastError();
        // A prior acknowledgment may have removed this marker already. The
        // transaction archive and Active remain the replay guard on that retry.
        if(error==ERROR_FILE_NOT_FOUND && retained)error=0;
        if(old_file!=INVALID_HANDLE_VALUE)CloseHandle(old_file);
        if(old)HeapFree(GetProcessHeap(),0,old);if(removed)CloseDirectory(removed);
    }
done:
    if(file!=INVALID_HANDLE_VALUE && !CloseHandle(file) && !error)error=GetLastError();
    if(state)AmdgpuWindowsStateClose(state);if(data)HeapFree(GetProcessHeap(),0,data);
    if(archive)CloseDirectory(archive);if(d)CloseDirectory(d);AmdgpuStateWriterUnlock(lock);return error;
}

static void RollbackLeaf(const BYTE tx[16],BOOL complete,WCHAR leaf[64])
{
    if(complete==2){(void)StringCchCopyW(leaf,64,L"\\rollback-active.bin");return;}
    WCHAR archive[46];TransactionLeaf(tx,archive);
    (void)StringCchPrintfW(leaf,64,L"\\rollback-%s%s",complete?L"done-":L"",archive+9);
}
DWORD AmdgpuWindowsRollbackRead(const BYTE tx[16],BOOL complete,void **data,DWORD *bytes)
{
    HANDOFF_DIRECTORY *d=NULL;HANDLE file=INVALID_HANDLE_VALUE,lock=NULL;WCHAR leaf[64];
    LARGE_INTEGER size;DWORD error,got=0;void *out=NULL;
    *data=NULL;*bytes=0;error=AmdgpuStateWriterLock(&lock);if(error)return error;
    error=Directory(&d);if(error)goto done;RollbackLeaf(tx,complete,leaf);
    if(FAILED(StringCchCatW(d->path,32768,leaf))){error=ERROR_FILENAME_EXCED_RANGE;goto done;}
    file=CreateFileW(d->path,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE){error=GetLastError();goto done;}
    error=Secure(file,FALSE);if(error)goto done;
    if(!GetFileSizeEx(file,&size)){error=GetLastError();goto done;}
    if(size.QuadPart<(LONGLONG)(AMDGPU_ROLLBACK_PREFIX+sizeof(AMDGPU_TRANSFER_HEADER)) || size.QuadPart>AMDGPU_ROLLBACK_MAX){error=ERROR_INVALID_DATA;goto done;}
    out=HeapAlloc(GetProcessHeap(),0,(SIZE_T)size.QuadPart);if(!out){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
    if(!ReadFile(file,out,(DWORD)size.QuadPart,&got,NULL)){error=GetLastError();goto done;}
    error=got!=(DWORD)size.QuadPart?ERROR_READ_FAULT:AmdgpuRollbackCheck(out,got);
    if(!error && tx && memcmp(((AMDGPU_ROLLBACK*)out)->snapshot.transaction,tx,16))error=ERROR_REVISION_MISMATCH;
    if(!error){*data=out;*bytes=got;out=NULL;}
done:
    if(out)HeapFree(GetProcessHeap(),0,out);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    if(d)CloseDirectory(d);AmdgpuStateWriterUnlock(lock);return error;
}
DWORD AmdgpuWindowsRollbackClear(const void *data,DWORD bytes)
{
    HANDOFF_DIRECTORY *d=NULL;HANDLE lock=NULL,file=INVALID_HANDLE_VALUE;DWORD error;FILE_DISPOSITION_INFO dispose={TRUE};
    error=AmdgpuRollbackCheck(data,bytes);if(error)return error;
    error=AmdgpuStateWriterLock(&lock);if(error)return error;
    error=Directory(&d);if(error)goto done;
    if(FAILED(StringCchCatW(d->path,32768,L"\\rollback-active.bin"))){error=ERROR_FILENAME_EXCED_RANGE;goto done;}
    file=CreateFileW(d->path,GENERIC_READ|READ_CONTROL|DELETE,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE){error=GetLastError();goto done;}
    error=Matches(file,data,bytes);if(!error && !SetFileInformationByHandle(file,FileDispositionInfo,&dispose,sizeof(dispose)))error=GetLastError();
done:
    if(file!=INVALID_HANDLE_VALUE && !CloseHandle(file) && !error)error=GetLastError();
    if(d)CloseDirectory(d);AmdgpuStateWriterUnlock(lock);return error;
}
DWORD AmdgpuWindowsRollbackSave(const void *data,DWORD bytes,BOOL complete)
{
    const AMDGPU_ROLLBACK *r=data;HANDOFF_DIRECTORY *d=NULL;HANDLE lock=NULL,file=INVALID_HANDLE_VALUE;
    WCHAR leaf[64];DWORD error,wrote=0;
    error=AmdgpuRollbackCheck(data,bytes);if(error)return error;
    error=AmdgpuStateWriterLock(&lock);if(error)return error;
    error=Directory(&d);if(error)goto done;RollbackLeaf(r->snapshot.transaction,complete,leaf);
    if(FAILED(StringCchCatW(d->path,32768,leaf))){error=ERROR_FILENAME_EXCED_RANGE;goto done;}
    file=CreateFileW(d->path,GENERIC_READ|GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file!=INVALID_HANDLE_VALUE) {error=Matches(file,data,bytes);if(!error && !FlushFileBuffers(file))error=GetLastError();goto done;}
    error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND)goto done;
    file=CreateFileW(d->path,GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,&d->sa,CREATE_NEW,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,NULL);
    if(file==INVALID_HANDLE_VALUE){error=GetLastError();goto done;}
    error=Secure(file,FALSE);if(error)goto done;
    if(!WriteFile(file,data,bytes,&wrote,NULL)){error=GetLastError();goto done;}
    if(wrote!=bytes){error=ERROR_WRITE_FAULT;goto done;}
    if(!FlushFileBuffers(file)){error=GetLastError();goto done;}
    {LARGE_INTEGER zero={0};if(!SetFilePointerEx(file,zero,NULL,FILE_BEGIN)){error=GetLastError();goto done;}}
    error=Matches(file,data,bytes);
done:
    if(file!=INVALID_HANDLE_VALUE && !CloseHandle(file) && !error)error=GetLastError();
    if(d)CloseDirectory(d);AmdgpuStateWriterUnlock(lock);return error;
}

// Fixed BoardMemory pair, read under the same protected ancestor discipline as
// the KMD. Never synthesize a fresh original on rollback, including EMPTY stores.
static DWORD BoardRead(HANDOFF_DIRECTORY *d,PCWSTR name,void *data,DWORD bytes)
{
    WCHAR path[32768];HANDLE file;DWORD error,got=0;LARGE_INTEGER size={0};
    if(FAILED(StringCchPrintfW(path,32768,L"%s\\%s",d->path,name)))return ERROR_FILENAME_EXCED_RANGE;
    file=CreateFileW(path,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE)return GetLastError();error=Secure(file,FALSE);
    if(!error && !GetFileSizeEx(file,&size))error=GetLastError();
    if(!error && size.QuadPart!=bytes)error=ERROR_INVALID_DATA;
    if(!error && !ReadFile(file,data,bytes,&got,NULL))error=GetLastError();
    if(!error && got!=bytes)error=ERROR_READ_FAULT;CloseHandle(file);return error;
}
DWORD AmdgpuWindowsRollbackBoard(void)
{
    HANDOFF_DIRECTORY *d=NULL;HANDLE board=NULL;BOARD_STORE_RECORD original,old;
    BOARD_STORE_JOURNAL journal;DWORD error,type,bytes,i;HKEY key=NULL;PSECURITY_DESCRIPTOR sd=NULL;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,FALSE};DWORD disposition;
    error=Directory(&d);if(error)return error;
    // Replace only the final fixed StateV2 leaf. Both parent handles stay held.
    *wcsrchr(d->path,L'\\')=0;
    if(FAILED(StringCchCatW(d->path,32768,L"\\BoardMemory"))){error=ERROR_FILENAME_EXCED_RANGE;goto done;}
    board=CreateFileW(d->path,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(board==INVALID_HANDLE_VALUE){board=NULL;error=GetLastError();goto done;}
    error=Secure(board,TRUE);if(error)goto done;
    error=BoardRead(d,L"original.dat",&original,sizeof(original));if(error)goto done;
    error=BoardRead(d,L"journal.dat",&journal,sizeof(journal));if(error)goto done;
    if(original.Magic!=BOARD_STORE_MAGIC || original.Version!=BOARD_STORE_VERSION || original.Bytes!=sizeof(original) ||
       original.Pending>1 || original.Checksum!=StoreChecksum(&original) ||
       journal.Magic!=BOARD_JOURNAL_MAGIC || journal.Version!=BOARD_JOURNAL_VERSION || journal.Bytes!=sizeof(journal) ||
       !journal.Sequence || journal.State!=BOARD_JOURNAL_CLEAR || journal.OriginalChecksum!=original.Checksum ||
       journal.Mapping!=original.Mapping || journal.BiosId!=original.BiosId || memcmp(journal.MachineId,original.MachineId,16) ||
       journal.Checksum!=JournalChecksum(&journal) || journal.Reserved[0] || journal.Reserved[1]){error=ERROR_INVALID_DATA;goto done;}
    for(i=0;i<4;i++)if(original.Reserved[i]){error=ERROR_INVALID_DATA;goto done;}
    original.Pending=0;original.Checksum=StoreChecksum(&original);
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;;KA;;;SY)(A;;KR;;;BA)",SDDL_REVISION_1,&sd,NULL)){error=GetLastError();goto done;}
    sa.lpSecurityDescriptor=sd;
    error=RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control\\amdgpu-wddm-board-memory",0,NULL,0,
        KEY_QUERY_VALUE|KEY_SET_VALUE|READ_CONTROL|KEY_WOW64_64KEY,&sa,&key,&disposition);if(error)goto done;
    // Existing backup may differ only in pending/checksum: its immutable origin
    // must agree. SYSTEM does not take ownership of an untrusted existing key.
    {PSECURITY_DESCRIPTOR actual=NULL;PSID owner,expectedOwner;PACL acl,expectedAcl;BOOL defaulted,present;DWORD revision;
     SECURITY_DESCRIPTOR_CONTROL control;DWORD aclBytes=0;
     error=GetSecurityInfo(key,SE_REGISTRY_KEY,OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,NULL,&acl,NULL,&actual);
     GetSecurityDescriptorOwner(sd,&expectedOwner,&defaulted);GetSecurityDescriptorDacl(sd,&present,&expectedAcl,&defaulted);
     if(!error && (!owner || !EqualSid(owner,expectedOwner) || !acl || !GetSecurityDescriptorControl(actual,&control,&revision) ||
        !(control&SE_DACL_PROTECTED) || acl->AceCount!=2))error=ERROR_INVALID_SECURITY_DESCR;
     if(!error){aclBytes=expectedAcl->AclSize;if(acl->AclSize!=aclBytes || memcmp(acl,expectedAcl,aclBytes))error=ERROR_INVALID_SECURITY_DESCR;}
     if(actual)LocalFree(actual);if(error)goto done;}
    type=0;bytes=sizeof(old);error=RegQueryValueExW(key,L"Backup",NULL,&type,(BYTE*)&old,&bytes);
    if(!error) {
        if(type!=REG_BINARY || bytes!=sizeof(old) || old.Checksum!=StoreChecksum(&old)){error=ERROR_INVALID_DATA;goto done;}
        old.Pending=0;old.Checksum=StoreChecksum(&old);
        if(memcmp(&old,&original,sizeof(old))){error=ERROR_REVISION_MISMATCH;goto done;}
    } else if(error!=ERROR_FILE_NOT_FOUND || disposition!=REG_CREATED_NEW_KEY)goto done;
    error=RegSetValueExW(key,L"Backup",0,REG_BINARY,(BYTE*)&original,sizeof(original));if(error)goto done;
    error=RegFlushKey(key);if(error)goto done;
    bytes=sizeof(old);error=RegQueryValueExW(key,L"Backup",NULL,&type,(BYTE*)&old,&bytes);
    if(!error && (type!=REG_BINARY || bytes!=sizeof(old) || memcmp(&old,&original,sizeof(old))))error=ERROR_INVALID_DATA;
done:
    if(key)RegCloseKey(key);if(sd)LocalFree(sd);if(board)CloseHandle(board);if(d)CloseDirectory(d);return error;
}
