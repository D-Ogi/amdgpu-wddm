// Real registry adapter exercised in one newly-created, volatile HKCU test tree.
// Service-root, installed-device lookup and the random Local mutex's ACL are
// injected. The test mutex belongs only to this user; production requires SYSTEM
// or Administrators. No service, device, installed setting or KMD is changed.
// This does not prove deployment SCM/mutex ACLs or restricted-token acceptance.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <strsafe.h>
#include <sddl.h>
#include <aclapi.h>
#include <objbase.h>
#include "state_windows.h"
#include "windows_migration.h"
#include "client.h"

static unsigned int checks,failures;
static HKEY Fixture;
static WCHAR Package[32768];
static DWORD RootError;
static WCHAR MutexName[100];
const WCHAR *AmdgpuTestWriterMutexName(void) {return MutexName;}
DWORD AmdgpuTestWriterMutexSecurity(PSECURITY_DESCRIPTOR *out)
{
    HANDLE token=NULL;DWORD needed=0,error=0;PWSTR sid=NULL;WCHAR sddl[256];
    union {void *alignment;BYTE bytes[sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE];} buffer;
    *out=NULL;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return GetLastError();
    if(!GetTokenInformation(token,TokenUser,buffer.bytes,sizeof(buffer.bytes),&needed))error=GetLastError();
    if(!error && !ConvertSidToStringSidW(((TOKEN_USER*)buffer.bytes)->User.Sid,&sid))error=GetLastError();
    if(!error && FAILED(StringCchPrintfW(sddl,256,L"D:P(A;;0x1f0001;;;%s)",sid)))error=ERROR_INVALID_DATA;
    if(!error && !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,out,NULL))error=GetLastError();
    if(sid)LocalFree(sid);CloseHandle(token);return error;
}
DWORD AmdgpuTestCheckWriterMutexSecurity(HANDLE mutex)
{
    PSECURITY_DESCRIPTOR expected=NULL,actual=NULL;PACL a=NULL,b=NULL;BOOL present=FALSE,defaults=FALSE;
    DWORD error=AmdgpuTestWriterMutexSecurity(&expected),revision=0;SECURITY_DESCRIPTOR_CONTROL control=0;
    if(!error)error=GetSecurityInfo(mutex,SE_KERNEL_OBJECT,DACL_SECURITY_INFORMATION,NULL,NULL,&a,NULL,&actual);
    if(!error && (!GetSecurityDescriptorDacl(expected,&present,&b,&defaults) || !present || !a || !b ||
       !GetSecurityDescriptorControl(actual,&control,&revision) || !(control&SE_DACL_PROTECTED) ||
       a->AclSize!=b->AclSize || memcmp(a,b,a->AclSize)))error=ERROR_INVALID_SECURITY_DESCR;
    if(expected)LocalFree(expected);if(actual)LocalFree(actual);return error;
}
#define CHECK(x) do { ++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);} } while(0)
DWORD AmdgpuTestSharedRoot(REGSAM access,HKEY *key)
{ return RootError?RootError:RegOpenKeyExW(Fixture,L"",0,access,key); }
unsigned int AmdgpuTestCatalogPath(WCHAR *path,DWORD chars,DWORD *error)
{ *error=0;return SUCCEEDED(StringCchCopyW(path,chars,Package))?0:AMDGPU_STATE_BUFFER; }
static void Number(HKEY key,const WCHAR *name,DWORD value)
{ CHECK(RegSetValueExW(key,name,0,REG_DWORD,(BYTE*)&value,4)==ERROR_SUCCESS); }
static void String(HKEY key,const WCHAR *name,const WCHAR *value)
{ CHECK(RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)value,(DWORD)(2*(wcslen(value)+1)))==ERROR_SUCCESS); }
static HKEY Child(HKEY parent,const WCHAR *name)
{
    HKEY key=NULL;
    CHECK(RegCreateKeyExW(parent,name,0,NULL,REG_OPTION_VOLATILE,KEY_ALL_ACCESS,NULL,&key,NULL)==0);
    return key;
}
typedef struct BATCH_RACE {
    AMDGPU_WINDOWS_STATE *state;
    AMDGPU_CLIENT_BATCH *batch;
    HANDLE entered,done;
    DWORD lock_wait,wrong_write,wrong_end,error;
    unsigned int freeze,capture;
    DWORD cpu,max;
} BATCH_RACE;
static DWORD WINAPI BatchFreezer(void *context)
{
    BATCH_RACE *r=context;HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,MutexName);
    BYTE transaction[16]={7};DWORD choice=1;AMDGPU_TRANSFER_HEADER h={0};AMDGPU_STATE_VALUE value;
    BYTE *buffer=HeapAlloc(GetProcessHeap(),0,AMDGPU_TRANSFER_MAX_BYTES);const void *payload;
    unsigned int bytes=0,i;
    r->wrong_write=AmdgpuClientBatchWrite(r->batch,"","CpuTune",REG_DWORD,&choice,4);
    r->wrong_end=AmdgpuClientEndBatch(r->batch);
    r->lock_wait=mutex?WaitForSingleObject(mutex,0):WAIT_FAILED;
    if(r->lock_wait==WAIT_OBJECT_0 || r->lock_wait==WAIT_ABANDONED)ReleaseMutex(mutex);
    if(mutex)CloseHandle(mutex);
    SetEvent(r->entered);
    r->freeze=AmdgpuWindowsStateFreeze(r->state,transaction,&r->error);
    h.origin=AMDGPU_TRANSFER_CURRENT;h.source_schema=2;h.transaction[0]=7;
    memset(h.source_generation,0x11,16);h.device_hash[0]=2;h.source_boot[0]=3;
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);
    strcpy_s(h.source_version,32,"0.7.216.101");strcpy_s(h.destination_version,32,"0.7.216.102");
    r->capture=buffer?AmdgpuWindowsCaptureCurrent(r->state,&h,buffer,AMDGPU_TRANSFER_MAX_BYTES,&bytes,&r->error):AMDGPU_STATE_IO;
    if(!r->capture)for(i=0;i<AmdgpuStateCount();i++) {
        if(AmdgpuTransferFind(buffer,bytes,i,&value,&payload)!=0 || value.bytes!=4)continue;
        if(!strcmp(AmdgpuStateAt(i)->name,"CpuTune"))memcpy(&r->cpu,payload,4);
        if(!strcmp(AmdgpuStateAt(i)->name,"DpmMaxMHz"))memcpy(&r->max,payload,4);
    }
    if(buffer)HeapFree(GetProcessHeap(),0,buffer);
    SetEvent(r->done);return 0;
}
static void BatchTest(HKEY root)
{
    AMDGPU_CLIENT_BATCH *batch=NULL;AMDGPU_WINDOWS_STATE *state=NULL;AMDGPU_STATE_VALUE value;
    BATCH_RACE r={0};HANDLE thread;BYTE transaction[16]={7};DWORD error=0,choice=0,beforeCpu=0,beforeMax=0;
    CHECK(AmdgpuWindowsStateOpen(&state,1,&error)==0 && state);if(!state)return;
    choice=1;CHECK(AmdgpuClientWrite(state,"","CpuTune",REG_DWORD,&choice,4)==0);
    choice=2000;CHECK(AmdgpuClientWrite(state,"","DpmMaxMHz",REG_DWORD,&choice,4)==0);
    CHECK(AmdgpuClientBeginBatch(&batch)==0 && batch);if(!batch)goto done;
    // Capture both old overrides on the retained generation before a write.
    CHECK(AmdgpuClientBatchRead(batch,"","CpuTune",0,&value,&beforeCpu,4)==0 && beforeCpu==1);
    CHECK(AmdgpuClientBatchRead(batch,"","DpmMaxMHz",0,&value,&beforeMax,4)==0 && beforeMax==2000);
    choice=0;error=AmdgpuClientBatchWrite(batch,"","CpuTune",REG_DWORD,&choice,4);
    if(error)printf("batch first write error %lu\n",error);CHECK(error==0);
    r.state=state;r.batch=batch;r.cpu=r.max=~0u;
    r.entered=CreateEventW(NULL,TRUE,FALSE,NULL);r.done=CreateEventW(NULL,TRUE,FALSE,NULL);
    thread=CreateThread(NULL,0,BatchFreezer,&r,0,NULL);CHECK(thread!=NULL);
    if(thread) {
        CHECK(WaitForSingleObject(r.entered,5000)==WAIT_OBJECT_0);
        CHECK(r.wrong_write==ERROR_INVALID_THREAD_ID && r.wrong_end==ERROR_INVALID_THREAD_ID);
        if(r.lock_wait!=WAIT_TIMEOUT)printf("batch competing mutex wait %lu\n",r.lock_wait);
        CHECK(r.lock_wait==WAIT_TIMEOUT && WaitForSingleObject(r.done,50)==WAIT_TIMEOUT);
    }
    // The second write is refused. Restore the first before releasing the
    // installer, which must capture the complete old group, never the prefix.
    choice=99999;CHECK(AmdgpuClientBatchWrite(batch,"","DpmMaxMHz",REG_DWORD,&choice,4)==ERROR_INVALID_DATA);
    CHECK(AmdgpuClientBatchWrite(batch,"","CpuTune",REG_DWORD,&beforeCpu,4)==0);
    CHECK(AmdgpuClientBatchWrite(batch,"","DpmMaxMHz",REG_DWORD,&beforeMax,4)==0);
    CHECK(AmdgpuClientBatchRead(batch,"","CpuTune",0,&value,&choice,4)==0 && choice==beforeCpu);
    CHECK(AmdgpuClientEndBatch(batch)==0);batch=NULL;
    if(thread) {
        CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
        CHECK(r.freeze==0 && r.capture==0 && r.cpu==beforeCpu && r.max==beforeMax);
        CloseHandle(thread);
    }
    CloseHandle(r.entered);CloseHandle(r.done);
    CHECK(AmdgpuClientBeginBatch(&batch)==ERROR_NOT_READY && !batch);
    CHECK(AmdgpuWindowsStateCancelFreeze(state,transaction,&error)==0);
    CHECK(AmdgpuClientBeginBatch(&batch)==0 && batch);
    if(batch) {
        CHECK(AmdgpuClientBatchClear(batch,"","CpuTune")==0);
        CHECK(AmdgpuClientBatchRead(batch,"","CpuTune",0,&value,&choice,4)==ERROR_FILE_NOT_FOUND);
        String(root,L"Active",L"22222222222222222222222222222222");
        CHECK(AmdgpuClientBatchWrite(batch,"","CpuTune",REG_DWORD,&beforeCpu,4)==ERROR_REVISION_MISMATCH);
        CHECK(AmdgpuClientBatchRead(batch,"","CpuTune",0,&value,&choice,4)==ERROR_REVISION_MISMATCH);
        CHECK(AmdgpuClientEndBatch(batch)==0);batch=NULL;
        String(root,L"Active",L"11111111111111111111111111111111");
    }
done:
    AmdgpuClientEndBatch(batch);AmdgpuWindowsStateClose(state);
}
static void CaptureTest(AMDGPU_WINDOWS_STATE *state)
{
    AMDGPU_TRANSFER_HEADER h={0};AMDGPU_STATE_VALUE value;const void *payload=NULL;
    BYTE *buffer=HeapAlloc(GetProcessHeap(),0,AMDGPU_TRANSFER_MAX_BYTES);
    BYTE other[16]={9};DWORD error=0,choice=0;unsigned int bytes=0,i,cpu=~0u;
    BYTE generation_id[16]={0};
    CHECK(buffer!=NULL);if(!buffer)return;
    h.origin=AMDGPU_TRANSFER_CURRENT;h.source_schema=2;h.transaction[0]=7;
    memset(h.source_generation,0x11,16);h.device_hash[0]=2;h.source_boot[0]=3;
    CHECK(AmdgpuWindowsStateIdentity(state,generation_id,&error)==0 &&
          !memcmp(generation_id,h.source_generation,16));
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);
    strcpy_s(h.source_version,32,"0.7.216.101");strcpy_s(h.destination_version,32,"0.7.216.102");
    CHECK(AmdgpuWindowsStateSet(state,"","CpuTune",REG_DWORD,&choice,4,&error)==0);
    CHECK(AmdgpuWindowsCaptureCurrent(state,&h,buffer,AMDGPU_TRANSFER_MAX_BYTES,&bytes,&error)==AMDGPU_STATE_INCOMPLETE && !bytes);
    CHECK(AmdgpuWindowsStateFreeze(state,h.transaction,&error)==0);
    CHECK(AmdgpuWindowsStateFreeze(state,h.transaction,&error)==0);
    CHECK(AmdgpuWindowsStateFreeze(state,other,&error)==AMDGPU_STATE_SCHEMA);
    CHECK(AmdgpuWindowsStateCancelFreeze(state,other,&error)==AMDGPU_STATE_SCHEMA);
    choice=1;CHECK(AmdgpuWindowsStateSet(state,"","CpuTune",REG_DWORD,&choice,4,&error)==AMDGPU_STATE_INCOMPLETE);
    CHECK(AmdgpuWindowsCaptureCurrent(state,&h,buffer,AMDGPU_TRANSFER_MAX_BYTES,&bytes,&error)==0);
    for(i=0;i<AmdgpuStateCount();i++)if(!strcmp(AmdgpuStateAt(i)->name,"CpuTune"))cpu=i;
    CHECK(AmdgpuTransferFind(buffer,bytes,cpu,&value,&payload)==0 && value.type==REG_DWORD && value.bytes==4);
    CHECK(payload && !memcmp(payload,"\0\0\0\0",4));
    h.source_generation[0]=1;
    CHECK(AmdgpuWindowsCaptureCurrent(state,&h,buffer,AMDGPU_TRANSFER_MAX_BYTES,&bytes,&error)==AMDGPU_STATE_SCHEMA && !bytes);
    CHECK(AmdgpuWindowsStateCancelFreeze(state,h.transaction,&error)==0);
    CHECK(AmdgpuWindowsStateSet(state,"","CpuTune",REG_DWORD,&choice,4,&error)==0);
    HeapFree(GetProcessHeap(),0,buffer);
}
static void DataTest(HKEY root,HKEY generation,HKEY data)
{
    AMDGPU_WINDOWS_STATE *state=NULL,*readonly=NULL; AMDGPU_STATE_VALUE value;
    DWORD error=0,out=0; unsigned int result;
    CHECK(AmdgpuWindowsStateOpen(&state,1,&error)==0 && state);
    CHECK(AmdgpuWindowsStateOpen(&readonly,0,&error)==0 && readonly);
    if(!state || !readonly) goto done;
    result=AmdgpuWindowsStateQuery(state,"","CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&value,&out,4);
    CHECK(result==0 && out==1 && value.source==AMDGPU_STATE_FACTORY);
    CHECK(AmdgpuWindowsStateQuery(state,"","CpuTune",AMDGPU_STATE_QUERY_OVERRIDE,&value,&out,4)==AMDGPU_STATE_ABSENT);
    out=0;CHECK(AmdgpuWindowsStateSet(state,"","CpuTune",REG_DWORD,&out,4,&error)==0);
    out=9;CHECK(AmdgpuWindowsStateQuery(state,"","CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&value,&out,4)==0 && out==0);
    out=1;CHECK(AmdgpuWindowsStateSet(readonly,"","CpuTune",REG_DWORD,&out,4,&error)==AMDGPU_STATE_ACCESS);
    CHECK(AmdgpuWindowsStateClear(readonly,"","CpuTune",&error)==AMDGPU_STATE_ACCESS);
    CHECK(AmdgpuWindowsStateClear(state,"","CpuTune",&error)==0);
    CHECK(AmdgpuWindowsStateQuery(state,"","CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&value,&out,4)==0 && out==1);
    String(data,L"CpuTune",L"1");
    CHECK(AmdgpuWindowsStateQuery(state,"","CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&value,&out,4)==AMDGPU_STATE_TYPE);
    CHECK(AmdgpuWindowsStateClear(state,"","CpuTune",&error)==0);
    out=2;CHECK(AmdgpuWindowsStateSet(state,"","CpuTune",REG_DWORD,&out,4,&error)==AMDGPU_STATE_RANGE);
    Number(data,L"CpuTune",2);
    CHECK(AmdgpuWindowsStateQuery(state,"","CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&value,&out,4)==AMDGPU_STATE_RANGE);
    Number(root,L"Frozen",1);out=0;
    CHECK(AmdgpuWindowsStateSet(state,"","CpuTune",REG_DWORD,&out,4,&error)==AMDGPU_STATE_INCOMPLETE);
    CHECK(AmdgpuWindowsStateClear(state,"","CpuTune",&error)==AMDGPU_STATE_INCOMPLETE);
    Number(root,L"Frozen",0);
    CHECK(AmdgpuWindowsStateClear(state,"","CpuTune",&error)==0);
    String(root,L"Active",L"22222222222222222222222222222222");
    CHECK(AmdgpuWindowsStateQuery(state,"","CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&value,&out,4)==AMDGPU_STATE_SCHEMA);
    CHECK(AmdgpuWindowsStateSet(state,"","CpuTune",REG_DWORD,&out,4,&error)==AMDGPU_STATE_SCHEMA);
    String(root,L"Active",L"11111111111111111111111111111111");
    CHECK(AmdgpuWindowsStateQuery(state,"","CpuPending",AMDGPU_STATE_READ_RECORD,&value,&out,4)==AMDGPU_STATE_ABSENT);
    Number(data,L"CpuPending",1);
    CHECK(AmdgpuWindowsStateQuery(state,"","CpuPending",AMDGPU_STATE_READ_RECORD,&value,&out,4)==0 && out==1);
    CHECK(AmdgpuWindowsStateSet(state,"","CpuPending",REG_DWORD,&out,4,&error)==AMDGPU_STATE_ACCESS);
    CHECK(AmdgpuWindowsStateQuery(state,"CuModeBoot","Valid",AMDGPU_STATE_READ_RECORD,&value,&out,4)==AMDGPU_STATE_ABSENT);
    CHECK(AmdgpuWindowsStateQuery(state,"..","CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&value,&out,4)==AMDGPU_STATE_UNKNOWN);
    CaptureTest(state);
    BatchTest(root);
done:
    AmdgpuWindowsStateClose(state);AmdgpuWindowsStateClose(readonly);
    state=NULL;
    Number(generation,L"Ready",0);
    CHECK(AmdgpuWindowsStateOpen(&state,0,&error)==AMDGPU_STATE_INCOMPLETE && !state);
    Number(generation,L"Ready",AmdgpuStateSchema());
    String(generation,L"CatalogId",L"wrong");
    CHECK(AmdgpuWindowsStateOpen(&state,0,&error)==AMDGPU_STATE_SCHEMA && !state);
    RootError=ERROR_ACCESS_DENIED;
    CHECK(AmdgpuWindowsStateOpen(&state,0,&error)==AMDGPU_STATE_ACCESS && !state && error==ERROR_ACCESS_DENIED);
    RootError=0;
}
static void RetireTest(HKEY root,HKEY generation)
{
    AMDGPU_WINDOWS_STATE *state=NULL,*readonly=NULL,*after=NULL;BYTE tx[16]={5},other[16]={6};
    DWORD error=0;HKEY keep=Child(Fixture,L"Unrelated"),check=NULL;WCHAR catalog[65];unsigned int i;
    Number(keep,L"Keep",77);
    for(i=0;i<65;i++)catalog[i]=(BYTE)AmdgpuStateCatalogId()[i];String(generation,L"CatalogId",catalog);
    CHECK(AmdgpuWindowsStateOpen(&state,1,&error)==0 && state);
    CHECK(AmdgpuWindowsStateOpen(&readonly,0,&error)==0 && readonly);
    if(!state || !readonly)goto done;
    CHECK(AmdgpuWindowsStateRetire(readonly,tx,&error)==AMDGPU_STATE_ACCESS);
    CHECK(AmdgpuWindowsStateRetire(state,tx,&error)==AMDGPU_STATE_INCOMPLETE);
    CHECK(AmdgpuWindowsStateFreeze(state,tx,&error)==0);
    CHECK(AmdgpuWindowsStateRetire(state,other,&error)==AMDGPU_STATE_SCHEMA);
    String(root,L"Active",L"22222222222222222222222222222222");
    CHECK(AmdgpuWindowsStateRetire(state,tx,&error)==AMDGPU_STATE_SCHEMA);
    String(root,L"Active",L"11111111111111111111111111111111");
    RootError=ERROR_ACCESS_DENIED;
    CHECK(AmdgpuWindowsStateRetire(state,tx,&error)==AMDGPU_STATE_ACCESS);
    RootError=0;CHECK(AmdgpuWindowsStateOpen(&after,0,&error)==0 && after);AmdgpuWindowsStateClose(after);after=NULL;
    CHECK(AmdgpuWindowsStateRetire(state,tx,&error)==0);
    CHECK(RegOpenKeyExW(Fixture,L"StateV2",0,KEY_READ,&check)==ERROR_FILE_NOT_FOUND && !check);
    CHECK(RegOpenKeyExW(Fixture,L"Unrelated",0,KEY_READ,&check)==0 && check);if(check)RegCloseKey(check);
    CHECK(AmdgpuWindowsStateOpen(&after,0,&error)==AMDGPU_STATE_INCOMPLETE && !after && error==ERROR_FILE_NOT_FOUND);
done:
    AmdgpuWindowsStateClose(state);AmdgpuWindowsStateClose(readonly);AmdgpuWindowsStateClose(after);RegCloseKey(keep);
}
#ifdef AMDGPU_STATE_GUI_FIXTURE
static HKEY GuiRoot,GuiGenerations,GuiGeneration,GuiData;
static WCHAR GuiPath[160];
__declspec(dllexport) DWORD WINAPI AmdgpuTestSetup(PCWSTR package)
{
    GUID guid;DWORD disposition=0;WCHAR catalog[65];unsigned int i;
    if(Fixture)return ERROR_INVALID_STATE;
    if(FAILED(StringCchCopyW(Package,32768,package)) || FAILED(CoCreateGuid(&guid)))return ERROR_INVALID_DATA;
    StringCchPrintfW(MutexName,100,L"Local\\amdgpu-wddm-test-%08lx-%lu",guid.Data1,GetCurrentProcessId());
    StringCchPrintfW(GuiPath,160,L"Software\\amdgpu-wddm-test-%08lx-%04x-%04x-%08lx",guid.Data1,guid.Data2,guid.Data3,GetCurrentProcessId());
    if(RegCreateKeyExW(HKEY_CURRENT_USER,GuiPath,0,NULL,REG_OPTION_VOLATILE,KEY_ALL_ACCESS,NULL,&Fixture,&disposition))return ERROR_ACCESS_DENIED;
    if(disposition!=REG_CREATED_NEW_KEY){RegCloseKey(Fixture);Fixture=NULL;return ERROR_ALREADY_EXISTS;}
    GuiRoot=Child(Fixture,L"StateV2");GuiGenerations=Child(GuiRoot,L"Generations");
    GuiGeneration=Child(GuiGenerations,L"11111111111111111111111111111111");GuiData=Child(GuiGeneration,L"Data");
    Number(GuiRoot,L"Frozen",0);String(GuiRoot,L"Active",L"11111111111111111111111111111111");
    Number(GuiGeneration,L"Ready",AmdgpuStateSchema());
    for(i=0;i<65;i++)catalog[i]=(BYTE)AmdgpuStateCatalogId()[i];String(GuiGeneration,L"CatalogId",catalog);
    return failures?ERROR_INVALID_DATA:0;
}
__declspec(dllexport) DWORD WINAPI AmdgpuTestCleanup(void)
{
    DWORD error;
    RegCloseKey(GuiData);RegCloseKey(GuiGeneration);RegCloseKey(GuiGenerations);RegCloseKey(GuiRoot);
    error=RegDeleteTreeW(Fixture,NULL);RegCloseKey(Fixture);Fixture=NULL;
    if(!error)error=RegDeleteKeyW(HKEY_CURRENT_USER,GuiPath);return error;
}
__declspec(dllexport) DWORD WINAPI AmdgpuTestFreeze(DWORD on)
{
    AMDGPU_WINDOWS_STATE *state=NULL;DWORD error=0;unsigned int result;BYTE tx[16]={7};
    result=AmdgpuWindowsStateOpen(&state,1,&error);
    if(!result)result=on?AmdgpuWindowsStateFreeze(state,tx,&error):AmdgpuWindowsStateCancelFreeze(state,tx,&error);
    AmdgpuWindowsStateClose(state);return error?error:result;
}
__declspec(dllexport) DWORD WINAPI AmdgpuTestNumber(PCWSTR name,DWORD value)
{return RegSetValueExW(GuiData,name,0,REG_DWORD,(BYTE*)&value,4);}
#endif
int wmain(int argc,WCHAR **argv)
{
    WCHAR path[160],id[65]; HKEY root=NULL,generations=NULL,generation=NULL,data=NULL;
    GUID guid; ULONG i; DWORD disposition=0;
    CHECK(argc==2);
    if(argc!=2) return 1;
    CHECK(SUCCEEDED(StringCchCopyW(Package,32768,argv[1])));
    CHECK(SUCCEEDED(CoCreateGuid(&guid)));
    CHECK(SUCCEEDED(StringCchPrintfW(MutexName,100,L"Local\\amdgpu-wddm-test-%08lx-%lu",guid.Data1,GetCurrentProcessId())));
    CHECK(SUCCEEDED(StringCchPrintfW(path,160,L"Software\\amdgpu-wddm-test-%08lx-%04x-%04x-%08lx",
        guid.Data1,guid.Data2,guid.Data3,GetCurrentProcessId())));
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER,path,0,NULL,REG_OPTION_VOLATILE,KEY_ALL_ACCESS,NULL,&Fixture,&disposition)==0);
    // Refuse even an extremely unlikely existing-name collision; never clear it.
    if(!Fixture || disposition!=REG_CREATED_NEW_KEY) { if(Fixture) RegCloseKey(Fixture); return 1; }
    root=Child(Fixture,L"StateV2");generations=Child(root,L"Generations");
    generation=Child(generations,L"11111111111111111111111111111111");data=Child(generation,L"Data");
    Number(root,L"Frozen",0);String(root,L"Active",L"11111111111111111111111111111111");
    Number(generation,L"Ready",AmdgpuStateSchema());
    for(i=0;i<65;i++) id[i]=(unsigned char)AmdgpuStateCatalogId()[i];
    String(generation,L"CatalogId",id);
    DataTest(root,generation,data);
    RetireTest(root,generation);
    RegCloseKey(data);RegCloseKey(generation);RegCloseKey(generations);RegCloseKey(root);
    CHECK(RegDeleteTreeW(Fixture,NULL)==0);RegCloseKey(Fixture);
    CHECK(RegDeleteKeyW(HKEY_CURRENT_USER,path)==0);
    printf("%u Windows adapter checks, %u failures\n",checks,failures);
    return failures?1:0;
}
