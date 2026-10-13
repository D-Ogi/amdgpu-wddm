// WDK/SDK 10.0.26100: winsvc.h GetSharedServiceRegistryStateKey.
// Parameters is installation-owned. No reconstructed service registry path.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <setupapi.h>
#include <devguid.h>
#include <strsafe.h>
#include "state_windows.h"
#include "windows_migration.h"
#include "windows_lock.h"
#ifdef AMDGPU_STATE_ADAPTER_TEST
extern unsigned int AmdgpuTestCatalogPath(WCHAR *,DWORD,DWORD *);
extern DWORD AmdgpuTestSharedRoot(REGSAM,HKEY *);
#endif

struct amdgpu_windows_state {
    HKEY root, generation, data, volatile_data;
    WCHAR transaction[33];
    BYTE *package;
    DWORD package_bytes;
    unsigned int writable;
    SRWLOCK lock;
};
static unsigned int Status(LSTATUS status)
{
    switch(status) {
    case ERROR_SUCCESS: return AMDGPU_STATE_OK;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return AMDGPU_STATE_ABSENT;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: return AMDGPU_STATE_ACCESS;
    case ERROR_MORE_DATA: case ERROR_INSUFFICIENT_BUFFER: return AMDGPU_STATE_BUFFER;
    default: return AMDGPU_STATE_IO;
    }
}
static unsigned int Name(const char *source, WCHAR *target, unsigned int capacity)
{
    unsigned int i;
    if(!source || !target || !capacity) return AMDGPU_STATE_INVALID_ARGUMENT;
    for(i=0; i<capacity; i++) {
        unsigned char c=(unsigned char)source[i];
        if(c>127) return AMDGPU_STATE_INVALID_ARGUMENT;
        target[i]=c;
        if(!c) return AMDGPU_STATE_OK;
    }
    return AMDGPU_STATE_INVALID_ARGUMENT;
}
static LSTATUS String(HKEY key, const WCHAR *name, WCHAR *out, DWORD capacity)
{
    DWORD type=0,bytes=capacity*sizeof(WCHAR),i;
    LSTATUS result=RegQueryValueExW(key,name,NULL,&type,(BYTE*)out,&bytes);
    if(result!=ERROR_SUCCESS) return result;
    if(type!=REG_SZ || bytes<sizeof(WCHAR) || bytes%sizeof(WCHAR) || bytes>capacity*sizeof(WCHAR) ||
       out[bytes/sizeof(WCHAR)-1]) return ERROR_INVALID_DATA;
    for(i=0;i+1<bytes/sizeof(WCHAR);i++) if(!out[i]) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
static LSTATUS Number(HKEY key, const WCHAR *name, DWORD *out)
{
    DWORD type=0,bytes=sizeof(*out);
    LSTATUS result=RegQueryValueExW(key,name,NULL,&type,(BYTE*)out,&bytes);
    return result==ERROR_SUCCESS && (type!=REG_DWORD || bytes!=sizeof(*out)) ? ERROR_INVALID_DATA : result;
}
static unsigned int Identity(AMDGPU_WINDOWS_STATE *s, DWORD *error, BOOL mutation)
{
    WCHAR active[33]; DWORD frozen=0; LSTATUS result;
    result=String(s->root,L"Active",active,33);
    *error=result;
    if(result!=ERROR_SUCCESS) return result==ERROR_FILE_NOT_FOUND ? AMDGPU_STATE_INCOMPLETE : Status(result);
    if(wcscmp(active,s->transaction)) return AMDGPU_STATE_SCHEMA;
    if(!mutation) return AMDGPU_STATE_OK;
    result=Number(s->root,L"Frozen",&frozen); *error=result;
    if(result!=ERROR_SUCCESS) return Status(result)==AMDGPU_STATE_ABSENT ? AMDGPU_STATE_INCOMPLETE : Status(result);
    return frozen ? AMDGPU_STATE_INCOMPLETE : AMDGPU_STATE_OK;
}
unsigned int AmdgpuWindowsStateCheckWrite(AMDGPU_WINDOWS_STATE *s,DWORD *error)
{
    unsigned int result;
    if(!s || !error)return AMDGPU_STATE_INVALID_ARGUMENT;
    *error=0;if(!s->writable)return AMDGPU_STATE_ACCESS;
    AcquireSRWLockShared(&s->lock);
    result=Identity(s,error,TRUE);
    ReleaseSRWLockShared(&s->lock);return result;
}
// Resolve only the installed software key for the service. A failed device need
// not expose a device interface. Multiple bindings must name the same catalog.
static unsigned int PackagePath(WCHAR *path, DWORD capacity, DWORD *error)
{
#ifdef AMDGPU_STATE_ADAPTER_TEST
    return AmdgpuTestCatalogPath(path,capacity,error);
#else
    HDEVINFO devices; SP_DEVINFO_DATA device; DWORD index; BOOL found=FALSE; WCHAR *candidate;
    unsigned int result=AMDGPU_STATE_ABSENT;
    devices=SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,NULL,NULL,0);
    if(devices==INVALID_HANDLE_VALUE) { *error=GetLastError(); return Status(*error); }
    candidate=(WCHAR*)HeapAlloc(GetProcessHeap(),0,32768*sizeof(WCHAR));
    if(!candidate) {
        SetupDiDestroyDeviceInfoList(devices);*error=ERROR_NOT_ENOUGH_MEMORY;return AMDGPU_STATE_IO;
    }
    ZeroMemory(&device,sizeof(device)); device.cbSize=sizeof(device);
    for(index=0;;index++) {
        WCHAR service[256]; HKEY key; DWORD type=0,needed=0; LSTATUS status;
        if(!SetupDiEnumDeviceInfo(devices,index,&device)) {
            *error=GetLastError();
            if(*error!=ERROR_NO_MORE_ITEMS) result=Status(*error);
            break;
        }
        ZeroMemory(service,sizeof(service));
        if(!SetupDiGetDeviceRegistryPropertyW(devices,&device,SPDRP_SERVICE,&type,(BYTE*)service,
                                              sizeof(service),&needed)) continue;
        if(type!=REG_SZ || needed<2 || needed>sizeof(service) || needed%2 || service[needed/2-1] ||
           _wcsicmp(service,L"bc250kmd")) continue;
        key=SetupDiOpenDevRegKey(devices,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
        if(key==INVALID_HANDLE_VALUE) { *error=GetLastError(); result=Status(*error); break; }
        status=String(key,L"AmdgpuStateCatalog",candidate,32768);
        RegCloseKey(key); *error=status;
        if(status!=ERROR_SUCCESS) { result=Status(status); break; }
        if(found && wcscmp(path,candidate)) { result=AMDGPU_STATE_SCHEMA; break; }
        if(FAILED(StringCchCopyW(path,capacity,candidate))) { result=AMDGPU_STATE_BUFFER; break; }
        found=TRUE; result=AMDGPU_STATE_OK;
    }
    SetupDiDestroyDeviceInfoList(devices);
    HeapFree(GetProcessHeap(),0,candidate);
    if(result==AMDGPU_STATE_OK) *error=ERROR_SUCCESS;
    return result;
#endif
}
static unsigned int Package(AMDGPU_WINDOWS_STATE *s, DWORD *error)
{
    WCHAR *path; HANDLE file; LARGE_INTEGER size; DWORD got=0; unsigned int result;
    if(s->package) return AMDGPU_STATE_OK;
    path=(WCHAR*)HeapAlloc(GetProcessHeap(),0,32768*sizeof(WCHAR));
    if(!path) { *error=ERROR_NOT_ENOUGH_MEMORY; return AMDGPU_STATE_IO; }
    result=PackagePath(path,32768,error);
    if(result!=AMDGPU_STATE_OK) { HeapFree(GetProcessHeap(),0,path); return result; }
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    HeapFree(GetProcessHeap(),0,path);
    if(file==INVALID_HANDLE_VALUE) { *error=GetLastError(); return Status(*error); }
    {
        FILE_ATTRIBUTE_TAG_INFO tag;
        if(!GetFileInformationByHandleEx(file,FileAttributeTagInfo,&tag,sizeof(tag))) {
            *error=GetLastError(); result=Status(*error);
        } else if(tag.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) result=AMDGPU_STATE_SCHEMA;
        else if(!GetFileSizeEx(file,&size)) { *error=GetLastError(); result=Status(*error); }
        else if(size.QuadPart!=AmdgpuStatePackageBytes()) result=AMDGPU_STATE_SCHEMA;
        else {
            s->package_bytes=(DWORD)size.QuadPart;
            s->package=(BYTE*)HeapAlloc(GetProcessHeap(),0,s->package_bytes);
            if(!s->package) { *error=ERROR_NOT_ENOUGH_MEMORY; result=AMDGPU_STATE_IO; }
            else if(!ReadFile(file,s->package,s->package_bytes,&got,NULL)) { *error=GetLastError(); result=Status(*error); }
            else if(got!=s->package_bytes) result=AMDGPU_STATE_INCOMPLETE;
            else result=AmdgpuStatePackageCheck(s->package,s->package_bytes);
        }
    }
    CloseHandle(file);
    if(result!=AMDGPU_STATE_OK && s->package) { HeapFree(GetProcessHeap(),0,s->package); s->package=NULL; }
    return result;
}
static DWORD SharedRoot(REGSAM access,HKEY *shared)
{
#ifdef AMDGPU_STATE_ADAPTER_TEST
    return AmdgpuTestSharedRoot(access,shared);
#else
    SC_HANDLE manager,service; DWORD error;
    manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);
    if(!manager) return GetLastError();
    service=OpenServiceW(manager,L"bc250kmd",SERVICE_QUERY_STATUS);
    if(!service) { error=GetLastError(); CloseServiceHandle(manager); return error; }
    error=GetSharedServiceRegistryStateKey(service,ServiceSharedRegistryPersistentState,access,shared);
    CloseServiceHandle(service); CloseServiceHandle(manager);
    return error;
#endif
}
unsigned int AmdgpuWindowsStateOpen(AMDGPU_WINDOWS_STATE **out,unsigned int writable,DWORD *error)
{
    HKEY shared=NULL; AMDGPU_WINDOWS_STATE *s=NULL;
    DWORD ready=0,i; WCHAR child[80],catalog[65],expected[65]; unsigned int result=AMDGPU_STATE_IO;
    REGSAM access=KEY_READ|(writable?KEY_WRITE:0); LSTATUS status;
    if(!out || !error || writable>1) return AMDGPU_STATE_INVALID_ARGUMENT;
    *out=NULL; *error=ERROR_SUCCESS;
    s=(AMDGPU_WINDOWS_STATE*)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*s));
    if(!s) { *error=ERROR_NOT_ENOUGH_MEMORY; goto done; }
    s->writable=writable; InitializeSRWLock(&s->lock);
    *error=SharedRoot(access,&shared);
    if(*error!=ERROR_SUCCESS) goto done;
    status=RegOpenKeyExW(shared,L"StateV2",0,access,&s->root); *error=status;
    if(status!=ERROR_SUCCESS) goto done;
    status=String(s->root,L"Active",s->transaction,33); *error=status;
    if(status!=ERROR_SUCCESS) goto done;
    for(i=0;i<32;i++) if(!((s->transaction[i]>=L'0' && s->transaction[i]<=L'9') ||
                           (s->transaction[i]>=L'a' && s->transaction[i]<=L'f'))) { result=AMDGPU_STATE_SCHEMA; goto done; }
    if(s->transaction[32]) { result=AMDGPU_STATE_SCHEMA; goto done; }
    if(FAILED(StringCchPrintfW(child,80,L"Generations\\%s",s->transaction))) goto done;
    status=RegOpenKeyExW(s->root,child,0,access,&s->generation); *error=status;
    if(status!=ERROR_SUCCESS) goto done;
    status=Number(s->generation,L"Ready",&ready); *error=status;
    if(status!=ERROR_SUCCESS || ready!=AmdgpuStateSchema()) { result=AMDGPU_STATE_INCOMPLETE; goto done; }
    status=String(s->generation,L"CatalogId",catalog,65); *error=status;
    if(status!=ERROR_SUCCESS) goto done;
    (void)Name(AmdgpuStateCatalogId(),expected,65);
    if(wcscmp(catalog,expected)) { result=AMDGPU_STATE_SCHEMA; goto done; }
    status=RegOpenKeyExW(s->generation,L"Data",0,access,&s->data); *error=status;
    if(status!=ERROR_SUCCESS) goto done;
    // V is optional for postmortem access after a restart before KMD has run.
    status=RegOpenKeyExW(s->root,L"Volatile",0,access,&s->volatile_data);
    if(status!=ERROR_SUCCESS && status!=ERROR_FILE_NOT_FOUND) { *error=status; goto done; }
    *out=s; s=NULL; result=AMDGPU_STATE_OK; *error=ERROR_SUCCESS;
done:
    if(result==AMDGPU_STATE_IO && *error) result=Status(*error);
    if(result==AMDGPU_STATE_ABSENT) result=AMDGPU_STATE_INCOMPLETE;
    if(shared) RegCloseKey(shared);
    AmdgpuWindowsStateClose(s);
    return result;
}
void AmdgpuWindowsStateClose(AMDGPU_WINDOWS_STATE *s)
{
    if(!s) return;
    if(s->volatile_data) RegCloseKey(s->volatile_data);
    if(s->data) RegCloseKey(s->data);
    if(s->generation) RegCloseKey(s->generation);
    if(s->root) RegCloseKey(s->root);
    if(s->package) HeapFree(GetProcessHeap(),0,s->package);
    HeapFree(GetProcessHeap(),0,s);
}
static unsigned int Fetch(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *v,void *data,unsigned int capacity)
{
    AMDGPU_WINDOWS_STATE *s=(AMDGPU_WINDOWS_STATE*)context;
    HKEY root,key=NULL; WCHAR path[128],name[128]; DWORD bytes=capacity,type=0,error=0; unsigned int result;
    LSTATUS status;
    if(source==AMDGPU_STATE_FACTORY) {
        result=Package(s,&error); v->os_error=error;
        if(result!=AMDGPU_STATE_OK) return result;
        return AmdgpuStatePackageRead(s->package,s->package_bytes,spec,v,data,capacity);
    }
    if(Name(spec->path,path,128) || Name(spec->name,name,128)) return AMDGPU_STATE_INVALID_ARGUMENT;
    root=spec->kind==AMDGPU_STATE_VOLATILE?s->volatile_data:s->data;
    if(!root) return AMDGPU_STATE_ABSENT;
    status=RegOpenKeyExW(root,path,0,KEY_QUERY_VALUE,&key);
    if(status==ERROR_SUCCESS) {
        status=RegQueryValueExW(key,name,NULL,&type,(BYTE*)data,&bytes);
        RegCloseKey(key);
    }
    v->os_error=status; v->type=type; v->bytes=bytes;
    return Status(status);
}
unsigned int AmdgpuWindowsStateQuery(AMDGPU_WINDOWS_STATE *s,const char *path,const char *name,
    unsigned int mode,AMDGPU_STATE_VALUE *v,void *data,unsigned int capacity)
{
    unsigned int result; DWORD error=0;
    if(!s || !v) return AMDGPU_STATE_INVALID_ARGUMENT;
    AcquireSRWLockExclusive(&s->lock);
    result=Identity(s,&error,FALSE);
    if(!result) result=AmdgpuStateQuery(AmdgpuStateFind(path,name),mode,Fetch,s,v,data,capacity);
    else { ZeroMemory(v,sizeof(*v)); v->status=result; v->os_error=error; }
    if(!result) {
        result=Identity(s,&error,FALSE);
        if(result) {
            if(data && capacity)SecureZeroMemory(data,capacity);
            ZeroMemory(v,sizeof(*v));v->status=result;v->os_error=error;
        }
    }
    ReleaseSRWLockExclusive(&s->lock);
    return result;
}
static unsigned int Change(AMDGPU_WINDOWS_STATE *s,const char *path,const char *name,
    unsigned int type,const void *data,unsigned int bytes,BOOL clear,DWORD *error)
{
    const AMDGPU_STATE_SPEC *spec=AmdgpuStateFind(path,name);
    WCHAR wide_path[128],wide_name[128]; HKEY key=NULL; LSTATUS status;
    HANDLE writer=NULL;
    unsigned int result;
    if(!s || !error) return AMDGPU_STATE_INVALID_ARGUMENT;
    *error=ERROR_SUCCESS;
    if(!spec) return AMDGPU_STATE_UNKNOWN;
    if(!s->writable || spec->kind==AMDGPU_STATE_VOLATILE ||
       (spec->access!=AMDGPU_STATE_ADMIN && !(clear && spec->access==AMDGPU_STATE_ADMIN_CLEAR)))
        return AMDGPU_STATE_ACCESS;
    if(!clear) { result=AmdgpuStateMayWrite(spec,1,0,type,data,bytes); if(result) return result; }
    else if(spec->kind!=AMDGPU_STATE_SETTING && spec->access!=AMDGPU_STATE_ADMIN_CLEAR) return AMDGPU_STATE_ACCESS;
    if(Name(path,wide_path,128) || Name(name,wide_name,128)) return AMDGPU_STATE_INVALID_ARGUMENT;
    *error=AmdgpuStateWriterLock(&writer);
    if(*error) return Status(*error);
    AcquireSRWLockExclusive(&s->lock);
    result=Identity(s,error,TRUE);
    if(!result) {
        status=RegOpenKeyExW(s->data,wide_path,0,KEY_SET_VALUE|KEY_QUERY_VALUE,&key);
        if(status==ERROR_SUCCESS) {
            status=clear?RegDeleteValueW(key,wide_name):RegSetValueExW(key,wide_name,0,type,(const BYTE*)data,bytes);
            if(clear && status==ERROR_FILE_NOT_FOUND) status=ERROR_SUCCESS;
            if(status==ERROR_SUCCESS) status=RegFlushKey(key);
            RegCloseKey(key);
        }
        *error=status; result=Status(status);
        if(!result) result=Identity(s,error,TRUE);
    }
    ReleaseSRWLockExclusive(&s->lock);
    AmdgpuStateWriterUnlock(writer);
    return result;
}
unsigned int AmdgpuWindowsStateSet(AMDGPU_WINDOWS_STATE *s,const char *path,const char *name,
    unsigned int type,const void *data,unsigned int bytes,DWORD *error)
{ return Change(s,path,name,type,data,bytes,FALSE,error); }
unsigned int AmdgpuWindowsStateClear(AMDGPU_WINDOWS_STATE *s,const char *path,const char *name,DWORD *error)
{ return Change(s,path,name,0,NULL,0,TRUE,error); }

unsigned int AmdgpuWindowsStateIdentity(AMDGPU_WINDOWS_STATE *s,unsigned char generation[16],DWORD *error)
{
    unsigned int result,i;
    if(!s || !generation || !error)return AMDGPU_STATE_INVALID_ARGUMENT;
    ZeroMemory(generation,16);*error=0;
    AcquireSRWLockShared(&s->lock);
    result=Identity(s,error,FALSE);
    if(!result) {
        for(i=0;i<32;i++) {
            WCHAR c=s->transaction[i];unsigned int digit;
            if(c>=L'0' && c<=L'9')digit=c-L'0';
            else if(c>=L'a' && c<=L'f')digit=c-L'a'+10;
            else {result=AMDGPU_STATE_SCHEMA;break;}
            generation[i/2]=(unsigned char)((generation[i/2]<<4)|digit);
        }
        if(!result)result=Identity(s,error,FALSE);
    }
    if(result)ZeroMemory(generation,16);
    ReleaseSRWLockShared(&s->lock);return result;
}
static unsigned int TransactionName(const unsigned char id[16],WCHAR text[33])
{
    static const WCHAR digits[]=L"0123456789abcdef";unsigned int i,any=0;
    if(!id) return AMDGPU_STATE_INVALID_ARGUMENT;
    for(i=0;i<16;i++) {text[2*i]=digits[id[i]>>4];text[2*i+1]=digits[id[i]&15];any|=id[i];}
    text[32]=0;return any?AMDGPU_STATE_OK:AMDGPU_STATE_INVALID_ARGUMENT;
}
static unsigned int FrozenOwner(AMDGPU_WINDOWS_STATE *s,const WCHAR tx[33],DWORD *error)
{
    WCHAR owner[33];DWORD frozen=0;
    *error=Number(s->root,L"Frozen",&frozen);
    if(*error) return Status(*error);
    if(frozen!=1) return AMDGPU_STATE_INCOMPLETE;
    *error=String(s->root,L"FrozenBy",owner,33);
    if(*error) return Status(*error);
    return wcscmp(owner,tx)?AMDGPU_STATE_SCHEMA:AMDGPU_STATE_OK;
}
unsigned int AmdgpuWindowsStateFreeze(AMDGPU_WINDOWS_STATE *s,const unsigned char transaction[16],DWORD *error)
{
    HANDLE writer=NULL;WCHAR tx[33];DWORD frozen=0;unsigned int result;
    if(!s || !error || !s->writable) return AMDGPU_STATE_ACCESS;
    *error=0;result=TransactionName(transaction,tx);if(result) return result;
    *error=AmdgpuStateWriterLock(&writer);if(*error) return Status(*error);
    AcquireSRWLockExclusive(&s->lock);
    result=Identity(s,error,FALSE);
    if(!result) {
        *error=Number(s->root,L"Frozen",&frozen);result=Status(*error);
        if(!result && frozen) result=FrozenOwner(s,tx,error);
        else if(!result) {
            *error=RegSetValueExW(s->root,L"FrozenBy",0,REG_SZ,(BYTE*)tx,sizeof(tx));
            if(!*error) *error=RegFlushKey(s->root);
            frozen=1;
            if(!*error) *error=RegSetValueExW(s->root,L"Frozen",0,REG_DWORD,(BYTE*)&frozen,4);
            if(!*error) *error=RegFlushKey(s->root);
            result=Status(*error);
        }
    }
    ReleaseSRWLockExclusive(&s->lock);AmdgpuStateWriterUnlock(writer);return result;
}
unsigned int AmdgpuWindowsStateCancelFreeze(AMDGPU_WINDOWS_STATE *s,const unsigned char transaction[16],DWORD *error)
{
    HANDLE writer=NULL;WCHAR tx[33];DWORD frozen=0;unsigned int result;
    if(!s || !error || !s->writable) return AMDGPU_STATE_ACCESS;
    *error=0;result=TransactionName(transaction,tx);if(result) return result;
    *error=AmdgpuStateWriterLock(&writer);if(*error) return Status(*error);
    AcquireSRWLockExclusive(&s->lock);
    result=Identity(s,error,FALSE);
    if(!result) result=FrozenOwner(s,tx,error);
    if(!result) {
        *error=RegSetValueExW(s->root,L"Frozen",0,REG_DWORD,(BYTE*)&frozen,4);
        if(!*error) *error=RegFlushKey(s->root);
        result=Status(*error);
    }
    ReleaseSRWLockExclusive(&s->lock);AmdgpuStateWriterUnlock(writer);return result;
}
unsigned int AmdgpuWindowsStateRetire(AMDGPU_WINDOWS_STATE *s,const unsigned char transaction[16],DWORD *error)
{
    HANDLE writer=NULL;HKEY shared=NULL,current=NULL;WCHAR tx[33];unsigned int result;
    AMDGPU_WINDOWS_STATE observed;
    if(!s || !error)return AMDGPU_STATE_INVALID_ARGUMENT;
    *error=0;if(!s->writable)return AMDGPU_STATE_ACCESS;
    result=TransactionName(transaction,tx);if(result)return result;
    *error=AmdgpuStateWriterLock(&writer);if(*error)return Status(*error);
    AcquireSRWLockExclusive(&s->lock);
    result=Identity(s,error,FALSE);
    if(!result)result=FrozenOwner(s,tx,error);
    if(!result){*error=SharedRoot(KEY_READ|KEY_WRITE|DELETE,&shared);result=Status(*error);}
    if(!result){*error=RegOpenKeyExW(shared,L"StateV2",0,KEY_READ,&current);result=Status(*error);}
    if(!result) {
        // Revalidate the child named for deletion under the newly opened SCM
        // parent; an old retained handle is never authority to delete new state.
        ZeroMemory(&observed,sizeof(observed));observed.root=current;
        CopyMemory(observed.transaction,s->transaction,sizeof(observed.transaction));
        result=Identity(&observed,error,FALSE);
        if(!result)result=FrozenOwner(&observed,tx,error);
    }
    if(!result) {
        *error=RegDeleteTreeW(shared,L"StateV2");result=Status(*error);
        if(!result){*error=RegFlushKey(shared);result=Status(*error);}
    }
    if(current)RegCloseKey(current);if(shared)RegCloseKey(shared);
    ReleaseSRWLockExclusive(&s->lock);AmdgpuStateWriterUnlock(writer);return result;
}
unsigned int AmdgpuWindowsCaptureCurrent(AMDGPU_WINDOWS_STATE *s,const AMDGPU_TRANSFER_HEADER *metadata,
    void *buffer,unsigned int capacity,unsigned int *bytes,DWORD *error)
{
    HANDLE writer=NULL;WCHAR tx[33],source[33];unsigned int result;
    if(!s || !metadata || !bytes || !error) return AMDGPU_STATE_INVALID_ARGUMENT;
    *bytes=0;*error=0;
    if(!s->writable) return AMDGPU_STATE_ACCESS;
    if(metadata->origin!=AMDGPU_TRANSFER_CURRENT) return AMDGPU_STATE_SCHEMA;
    result=TransactionName(metadata->transaction,tx);if(result) return result;
    result=TransactionName(metadata->source_generation,source);if(result) return result;
    *error=AmdgpuStateWriterLock(&writer);if(*error) return Status(*error);
    AcquireSRWLockExclusive(&s->lock);
    result=Identity(s,error,FALSE);
    if(!result && wcscmp(source,s->transaction)) result=AMDGPU_STATE_SCHEMA;
    if(!result) result=FrozenOwner(s,tx,error);
    if(!result) result=AmdgpuTransferCapture(metadata,Fetch,s,buffer,capacity,bytes);
    if(!result) result=Identity(s,error,FALSE);
    if(result) *bytes=0;
    ReleaseSRWLockExclusive(&s->lock);AmdgpuStateWriterUnlock(writer);return result;
}
