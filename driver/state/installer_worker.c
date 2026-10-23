// Narrow SYSTEM service for state export. It has no shell, caller-selected path,
// registry destination or arbitrary execution command. Never run in host tests.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <strsafe.h>
#include <stdio.h>
#include "windows_migration.h"
#include "windows_handoff.h"
#include "identity.h"
#include "installer_rollback.h"
#include <bcrypt.h>
#include <newdev.h>

#ifndef AMDGPU_INSTALLER_TEST
static const WCHAR ServiceName[]=L"amdgpu-wddm-state-transaction";
static SERVICE_STATUS_HANDLE StatusHandle;
static SERVICE_STATUS ServiceStatus;
#endif
static DWORD AsError(unsigned int status,DWORD error)
{return status?(error?error:ERROR_INVALID_DATA):ERROR_SUCCESS;}
static BOOL Hex(const WCHAR *text,BYTE out[16])
{
    DWORD i;if(wcslen(text)!=32)return FALSE;
    for(i=0;i<32;i++) {
        DWORD n=text[i]>=L'0' && text[i]<=L'9'?text[i]-L'0':
                text[i]>=L'a' && text[i]<=L'f'?text[i]-L'a'+10:16;
        if(n>15)return FALSE;
        if(!(i&1))out[i/2]=(BYTE)(n<<4);else out[i/2]|=(BYTE)n;
    }
    for(i=0;i<16;i++)if(out[i])return TRUE;return FALSE;
}
static BOOL Version(const WCHAR *text,char out[32])
{
    DWORD i,n=0,dots=0,digits=0;if(wcslen(text)>=32)return FALSE;
    ZeroMemory(out,32);
    for(i=0;text[i];i++) {
        if(text[i]==L'.'){if(!digits || dots==3)return FALSE;dots++;digits=0;n=0;}
        else if(text[i]>=L'0' && text[i]<=L'9') {
            n=n*10+text[i]-L'0';if(++digits>5 || n>65534)return FALSE;
        } else return FALSE;
        out[i]=(char)text[i];
    }
    return dots==3 && digits>0;
}
#ifdef AMDGPU_INSTALLER_TEST
extern DWORD AmdgpuInstallerTestDevice(PCWSTR,AMDGPU_TRANSFER_HEADER*,BOOL);
extern DWORD AmdgpuInstallerTestOrigin(UINT*);
extern DWORD AmdgpuInstallerTestTarget(PCWSTR,const AMDGPU_ROLLBACK*,BOOL);
extern DWORD AmdgpuInstallerTestQuiescent(PCWSTR);
extern DWORD AmdgpuInstallerTestSource(PCWSTR,AMDGPU_ROLLBACK*,BOOL);
#define Device AmdgpuInstallerTestDevice
#define Origin AmdgpuInstallerTestOrigin
#define Target AmdgpuInstallerTestTarget
#define Quiescent AmdgpuInstallerTestQuiescent
#define Source AmdgpuInstallerTestSource
extern ULONG AmdgpuInstallerTestBoot;
static ULONG BootId(void){return AmdgpuInstallerTestBoot;}
#else
static ULONG BootId(void){return *(volatile ULONG*)(ULONG_PTR)0x7ffe02c4;}
static DWORD KeyString(HKEY key,PCWSTR name,WCHAR out[32])
{
    DWORD type=0,bytes=64,error,i;
    error=RegQueryValueExW(key,name,NULL,&type,(BYTE*)out,&bytes);if(error)return error;
    if(type!=REG_SZ || bytes<2 || bytes>64 || bytes%2 || out[bytes/2-1])return ERROR_INVALID_DATA;
    for(i=0;i+1<bytes/2;i++)if(!out[i])return ERROR_INVALID_DATA;return 0;
}
static DWORD Device(PCWSTR id,AMDGPU_TRANSFER_HEADER *h,BOOL stop)
{
    HDEVINFO list;SP_DEVINFO_DATA device={sizeof(device)};DWORD error=0,type=0,bytes=0;
    WCHAR service[128];HKEY key=INVALID_HANDLE_VALUE;
    if(wcslen(id)>MAX_DEVICE_ID_LEN || AmdgpuStateDeviceIdentity((const unsigned short*)id,(UINT)wcslen(id),h->device_hash))
        return ERROR_INVALID_PARAMETER;
    list=SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,NULL,NULL,DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE)return GetLastError();
    if(!SetupDiOpenDeviceInfoW(list,id,NULL,0,&device)){error=GetLastError();goto done;}
    if(!SetupDiGetDeviceRegistryPropertyW(list,&device,SPDRP_SERVICE,&type,(BYTE*)service,sizeof(service),&bytes)) {
        error=GetLastError();if(error==ERROR_INVALID_DATA)error=0;goto done;
    }
    if(type!=REG_SZ || bytes<2 || bytes>sizeof(service) || bytes%2 || service[bytes/2-1]){error=ERROR_INVALID_DATA;goto done;}
    if(_wcsicmp(service,L"bc250kmd")) {
        if(stop)error=ERROR_SERVICE_NOT_ACTIVE;goto done;
    }
    key=SetupDiOpenDevRegKey(list,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_READ);
    if(key==INVALID_HANDLE_VALUE){error=GetLastError();goto done;}
    {
        WCHAR version[32];error=KeyString(key,L"ProductPackageVersion",version);
        if(error==ERROR_FILE_NOT_FOUND) {
            // Legacy private original version wins only when the current public
            // property equals the accompanying private reported property.
            WCHAR original[32],reported[32];
            error=KeyString(key,L"DriverVersion",version);if(error)goto done;
            if(!KeyString(key,L"Bc250DriverVersion",original) &&
               !KeyString(key,L"Bc250ReportedDriverVersion",reported) && !wcscmp(reported,version))
                (void)StringCchCopyW(version,32,original);
        }
        if(error)goto done;
        if(!Version(version,h->source_version)){error=ERROR_INVALID_DATA;goto done;}
        // Missing S in a package which declares the new catalog is damaged or
        // incomplete state, not a legacy installation and not a fresh machine.
        type=bytes=0;error=RegQueryValueExW(key,L"AmdgpuStateCatalog",NULL,&type,NULL,&bytes);
        if(!error) {
            if(type!=REG_SZ || bytes<4 || bytes>65536 || bytes%2){error=ERROR_INVALID_DATA;goto done;}
            h->source_schema=AmdgpuStateSchema();
        } else if(error==ERROR_FILE_NOT_FOUND)error=0;
        else goto done;
    }
    if(stop) {
        SP_PROPCHANGE_PARAMS change={0};ULONG status=0,problem=0;CONFIGRET cr;
        change.ClassInstallHeader.cbSize=sizeof(SP_CLASSINSTALL_HEADER);
        change.ClassInstallHeader.InstallFunction=DIF_PROPERTYCHANGE;
        change.StateChange=DICS_DISABLE;change.Scope=DICS_FLAG_GLOBAL;
        if(!SetupDiSetClassInstallParamsW(list,&device,&change.ClassInstallHeader,sizeof(change)) ||
           !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE,list,&device)){error=GetLastError();goto done;}
        cr=CM_Get_DevNode_Status(&status,&problem,device.DevInst,0);
        if(cr!=CR_SUCCESS || (status&DN_STARTED) || problem!=CM_PROB_DISABLED){error=ERROR_BUSY;goto done;}
        // Disabling a device alone is not proof that the service drained all
        // adapters. The independent SCM status must also say STOPPED.
        {
            SC_HANDLE manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT),driver=NULL;
            SERVICE_STATUS_PROCESS process;DWORD needed;
            if(!manager){error=GetLastError();goto done;}
            driver=OpenServiceW(manager,L"bc250kmd",SERVICE_QUERY_STATUS);
            if(!driver)error=GetLastError();
            else if(!QueryServiceStatusEx(driver,SC_STATUS_PROCESS_INFO,(BYTE*)&process,sizeof(process),&needed))error=GetLastError();
            else if(process.dwCurrentState!=SERVICE_STOPPED)error=ERROR_BUSY;
            if(driver)CloseServiceHandle(driver);CloseServiceHandle(manager);
        }
    }
done:
    if(key!=INVALID_HANDLE_VALUE)RegCloseKey(key);SetupDiDestroyDeviceInfoList(list);return error;
}
static DWORD Origin(UINT *origin)
{
    SC_HANDLE manager=NULL,service=NULL;HKEY shared=NULL,root=NULL;DWORD error;
    *origin=0;manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);if(!manager)return GetLastError();
    // The shared-state API needs a configuration-query handle. QUERY_STATUS
    // alone returns ACCESS_DENIED even for SYSTEM; that is not legacy absence.
    service=OpenServiceW(manager,L"bc250kmd",SERVICE_QUERY_CONFIG);
    if(!service) {
        error=GetLastError();if(error==ERROR_SERVICE_DOES_NOT_EXIST){*origin=AMDGPU_TRANSFER_FRESH;error=0;}
        goto done;
    }
    error=GetSharedServiceRegistryStateKey(service,ServiceSharedRegistryPersistentState,KEY_READ,&shared);
    if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND){*origin=AMDGPU_TRANSFER_LEGACY;error=0;goto done;}
    if(error)goto done;
    error=RegOpenKeyExW(shared,L"StateV2",0,KEY_READ,&root);
    if(error==ERROR_FILE_NOT_FOUND){*origin=AMDGPU_TRANSFER_LEGACY;error=0;goto done;}
    if(!error)*origin=AMDGPU_TRANSFER_CURRENT;
done:
    if(root)RegCloseKey(root);if(shared)RegCloseKey(shared);
    if(service)CloseServiceHandle(service);CloseServiceHandle(manager);return error;
}
#endif
static BOOL Hash(const WCHAR *text,BYTE out[32])
{
    WCHAR half[33];if(wcslen(text)!=64)return FALSE;
    memcpy(half,text,32*sizeof(WCHAR));half[32]=0;if(!Hex(half,out))return FALSE;
    return Hex(text+32,out+16);
}
#ifndef AMDGPU_INSTALLER_TEST
static DWORD Quiescent(PCWSTR id)
{
    HDEVINFO list;SP_DEVINFO_DATA device={sizeof(device)};ULONG status=0,problem=0;DWORD error=0,bytes;
    SC_HANDLE manager=NULL,service=NULL;SERVICE_STATUS_PROCESS process;
    list=SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,NULL,NULL,DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE)return GetLastError();
    if(!SetupDiOpenDeviceInfoW(list,id,NULL,0,&device)){error=GetLastError();goto done;}
    if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)!=CR_SUCCESS || (status&DN_STARTED) || problem!=CM_PROB_DISABLED){error=ERROR_BUSY;goto done;}
    manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);if(!manager){error=GetLastError();goto done;}
    service=OpenServiceW(manager,L"bc250kmd",SERVICE_QUERY_STATUS);
    if(!service){error=GetLastError();if(error==ERROR_SERVICE_DOES_NOT_EXIST)error=0;goto done;}
    if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE*)&process,sizeof(process),&bytes))error=GetLastError();
    else if(process.dwCurrentState!=SERVICE_STOPPED)error=ERROR_BUSY;
done:
    if(service)CloseServiceHandle(service);if(manager)CloseServiceHandle(manager);SetupDiDestroyDeviceInfoList(list);return error;
}
static DWORD FileDigest(PCWSTR path,BYTE digest[32])
{
    HANDLE file;BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE hash=NULL;
    BYTE data[8192];DWORD error=0,got;FILE_ATTRIBUTE_TAG_INFO tag;
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE)return GetLastError();
    if(!GetFileInformationByHandleEx(file,FileAttributeTagInfo,&tag,sizeof(tag))){error=GetLastError();goto done;}
    if(tag.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)){error=ERROR_REPARSE_TAG_INVALID;goto done;}
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0 ||
       BCryptCreateHash(algorithm,&hash,NULL,0,NULL,0,0)<0){error=ERROR_GEN_FAILURE;goto done;}
    for(;;){if(!ReadFile(file,data,sizeof(data),&got,NULL)){error=GetLastError();goto done;}if(!got)break;
        if(BCryptHashData(hash,data,got,0)<0){error=ERROR_GEN_FAILURE;goto done;}}
    if(BCryptFinishHash(hash,digest,32,0)<0)error=ERROR_GEN_FAILURE;
done:
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);CloseHandle(file);return error;
}
static DWORD FileHash(PCWSTR path,const BYTE expected[32])
{BYTE digest[32];DWORD error=FileDigest(path,digest);return error?error:memcmp(digest,expected,32)?ERROR_REVISION_MISMATCH:0;}
static DWORD Source(PCWSTR id,AMDGPU_ROLLBACK *r,BOOL remove)
{
    HDEVINFO list=INVALID_HANDLE_VALUE;SP_DEVINFO_DATA device={sizeof(device)};HKEY key=INVALID_HANDLE_VALUE;
    WCHAR windows[32768],path[32768];DWORD error=0;BOOL reboot=FALSE;
    if(!remove) {
        list=SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,NULL,NULL,DIGCF_PRESENT);if(list==INVALID_HANDLE_VALUE)return GetLastError();
        if(!SetupDiOpenDeviceInfoW(list,id,NULL,0,&device)){error=GetLastError();goto done;}
        key=SetupDiOpenDevRegKey(list,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
        if(key==INVALID_HANDLE_VALUE){error=GetLastError();goto done;}
        error=KeyString(key,L"InfPath",r->source_inf);if(error)goto done;
    }
    if(wcsncmp(r->source_inf,L"oem",3) || wcslen(r->source_inf)<8 || wcspbrk(r->source_inf,L"\\/:")){error=ERROR_INVALID_DATA;goto done;}
    if(!GetWindowsDirectoryW(windows,32768)){error=GetLastError();goto done;}
    if(FAILED(StringCchPrintfW(path,32768,L"%s\\INF\\%s",windows,r->source_inf))){error=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(!remove){error=FileDigest(path,r->source_inf_hash);goto done;}
    error=Quiescent(id);if(error)goto done;
    error=FileHash(path,r->source_inf_hash);
    if(error==ERROR_FILE_NOT_FOUND){error=0;goto done;}if(error)goto done;
    if(remove==2){error=ERROR_REVISION_MISMATCH;goto done;}
    // The protected capture names this exact formerly bound INF. No wildcard,
    // unrelated store cleanup, /force or caller-selected path is supported.
    if(!DiUninstallDriverW(NULL,path,0,&reboot)){error=GetLastError();goto done;}
    error=Quiescent(id);
done:
    if(key!=INVALID_HANDLE_VALUE)RegCloseKey(key);if(list!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(list);return error;
}
static DWORD Target(PCWSTR id,const AMDGPU_ROLLBACK *r,BOOL enable)
{
    HDEVINFO list=INVALID_HANDLE_VALUE;SP_DEVINFO_DATA device={sizeof(device)};HKEY key=INVALID_HANDLE_VALUE;
    WCHAR inf[32],path[32768],windows[32768];ULONG status=0,problem=0;DWORD error=0,needed;
    SC_HANDLE manager=NULL,service=NULL;BYTE configBuffer[8192];QUERY_SERVICE_CONFIGW *config=(void*)configBuffer;
    SERVICE_STATUS_PROCESS process;AMDGPU_TRANSFER_HEADER identity={0};
    error=Device(id,&identity,FALSE);if(error)return error;
    if(memcmp(identity.device_hash,r->snapshot.device_hash,32) || strcmp(identity.source_version,r->snapshot.destination_version) || identity.source_schema)
        return ERROR_REVISION_MISMATCH;
    list=SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,NULL,NULL,DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE)return GetLastError();
    if(!SetupDiOpenDeviceInfoW(list,id,NULL,0,&device)){error=GetLastError();goto done;}
    if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)!=CR_SUCCESS ||
       ((status&DN_STARTED)?enable<2:problem!=CM_PROB_DISABLED)){error=ERROR_BUSY;goto done;}
    if(enable==4 && !(status&DN_STARTED)){error=ERROR_BUSY;goto done;}
    manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);if(!manager){error=GetLastError();goto done;}
    service=OpenServiceW(manager,L"bc250kmd",SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG);if(!service){error=GetLastError();goto done;}
    if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE*)&process,sizeof(process),&needed)){error=GetLastError();goto done;}
    if(process.dwCurrentState!=SERVICE_STOPPED && !(enable>=2 && (status&DN_STARTED))){error=ERROR_BUSY;goto done;}
    key=SetupDiOpenDevRegKey(list,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
    if(key==INVALID_HANDLE_VALUE){error=GetLastError();goto done;}
    error=KeyString(key,L"InfPath",inf);if(error)goto done;
    if(wcslen(inf)<5 || _wcsicmp(inf+wcslen(inf)-4,L".inf") || wcspbrk(inf,L"\\/:")){error=ERROR_INVALID_DATA;goto done;}
    if(!GetWindowsDirectoryW(windows,32768)){error=GetLastError();goto done;}
    if(FAILED(StringCchPrintfW(path,32768,L"%s\\INF\\%s",windows,inf))){error=ERROR_FILENAME_EXCED_RANGE;goto done;}
    error=FileHash(path,r->inf_hash);if(error)goto done;
    if(!QueryServiceConfigW(service,config,sizeof(configBuffer),&needed)){error=GetLastError();goto done;}
    if(!_wcsnicmp(config->lpBinaryPathName,L"\\SystemRoot\\",12))
        error=FAILED(StringCchPrintfW(path,32768,L"%s\\%s",windows,config->lpBinaryPathName+12))?ERROR_FILENAME_EXCED_RANGE:0;
    else if(!_wcsnicmp(config->lpBinaryPathName,L"System32\\",9))
        error=FAILED(StringCchPrintfW(path,32768,L"%s\\%s",windows,config->lpBinaryPathName))?ERROR_FILENAME_EXCED_RANGE:0;
    else error=FAILED(StringCchCopyW(path,32768,config->lpBinaryPathName))?ERROR_FILENAME_EXCED_RANGE:0;
    if(error)goto done;
    {PCWSTR leaf=wcsrchr(path,L'\\');if(!leaf || _wcsicmp(leaf,L"\\bc250kmd.sys")){error=ERROR_INVALID_NAME;goto done;}}
    error=FileHash(path,r->sys_hash);if(error)goto done;
    if((enable==1 || enable==3) && !(status&DN_STARTED)){SP_PROPCHANGE_PARAMS change={0};change.ClassInstallHeader.cbSize=sizeof(SP_CLASSINSTALL_HEADER);
        change.ClassInstallHeader.InstallFunction=DIF_PROPERTYCHANGE;change.StateChange=DICS_ENABLE;change.Scope=DICS_FLAG_GLOBAL;
        if(!SetupDiSetClassInstallParamsW(list,&device,&change.ClassInstallHeader,sizeof(change)) ||
           !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE,list,&device))error=GetLastError();}
done:
    if(key!=INVALID_HANDLE_VALUE)RegCloseKey(key);if(service)CloseServiceHandle(service);if(manager)CloseServiceHandle(manager);
    if(list!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(list);return error;
}
#endif
static DWORD Rollback(DWORD argc,WCHAR **argv)
{
    AMDGPU_ROLLBACK requested={0},*r=NULL;AMDGPU_WINDOWS_STATE *state=NULL;void *pending=NULL,*complete=NULL;
    DWORD error,bytes=0,n=0,result;UINT origin,capture_bytes=0;BYTE generation[16],boot[16];BOOL capture,enable,active=FALSE;
    if(argc!=8 || !Hex(argv[2],requested.snapshot.transaction) || !Version(argv[3],requested.snapshot.destination_version) ||
       !Hash(argv[5],requested.inf_hash) || !Hash(argv[6],requested.sys_hash) || !Hash(argv[7],requested.manifest_hash))return ERROR_INVALID_PARAMETER;
    capture=!wcscmp(argv[1],L"rollback-capture");enable=!wcscmp(argv[1],L"rollback-enable");
    error=Device(argv[4],&requested.snapshot,FALSE);if(error)return error;
    error=AmdgpuWindowsReadHandoff(&pending,&n);
    if(!error){HeapFree(GetProcessHeap(),0,pending);return ERROR_BUSY;}if(error!=ERROR_FILE_NOT_FOUND)return error;
    error=AmdgpuWindowsRollbackRead(requested.snapshot.transaction,2,&pending,&n);
    if(!error){
        AMDGPU_ROLLBACK *a=pending;active=TRUE;
        if(memcmp(a->inf_hash,requested.inf_hash,96) || memcmp(a->snapshot.device_hash,requested.snapshot.device_hash,32) ||
           strcmp(a->snapshot.destination_version,requested.snapshot.destination_version))error=ERROR_REVISION_MISMATCH;
        else if(capture)error=AmdgpuWindowsRollbackSave(a,n,FALSE);
        HeapFree(GetProcessHeap(),0,pending);pending=NULL;if(error)return error;
    }
    else if(error!=ERROR_FILE_NOT_FOUND)return error;
    error=AmdgpuWindowsRollbackRead(requested.snapshot.transaction,FALSE,(void**)&r,&bytes);
    if(!error) {
        if(memcmp(r->inf_hash,requested.inf_hash,96) || memcmp(r->snapshot.device_hash,requested.snapshot.device_hash,32) ||
           strcmp(r->snapshot.destination_version,requested.snapshot.destination_version)){error=ERROR_REVISION_MISMATCH;goto done;}
        if(capture) {
            error=AmdgpuWindowsRollbackRead(r->snapshot.transaction,TRUE,&complete,&n);
            if(error==ERROR_FILE_NOT_FOUND)error=Quiescent(argv[4]);
            else if(!error && (n!=bytes || memcmp(complete,r,bytes)))error=ERROR_REVISION_MISMATCH;
            goto done;
        }
    } else if(error!=ERROR_FILE_NOT_FOUND || !capture)goto done;
    if(capture) {
        error=Origin(&origin);if(error)goto done;
        if(origin!=AMDGPU_TRANSFER_CURRENT || requested.snapshot.source_schema!=AmdgpuStateSchema()){error=ERROR_REVISION_MISMATCH;goto done;}
        result=AmdgpuWindowsStateOpen(&state,1,&error);if(result){error=AsError(result,error);goto done;}
        result=AmdgpuWindowsStateIdentity(state,requested.snapshot.source_generation,&error);if(result){error=AsError(result,error);goto done;}
        result=AmdgpuWindowsStateFreeze(state,requested.snapshot.transaction,&error);if(result){error=AsError(result,error);goto done;}
        error=Device(argv[4],&requested.snapshot,TRUE);if(error)goto done;
        error=AmdgpuWindowsRollbackBoard();if(error)goto done;
        r=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,AMDGPU_ROLLBACK_MAX);if(!r){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
        *r=requested;r->magic=AMDGPU_ROLLBACK_MAGIC;r->version=1;
        error=Source(argv[4],r,FALSE);if(error)goto done;
        requested.snapshot.origin=AMDGPU_TRANSFER_CURRENT;
        memcpy(requested.snapshot.catalog,AmdgpuStateCatalogId(),64);AmdgpuStateBootToken(BootId(),requested.snapshot.source_boot);
        result=AmdgpuWindowsCaptureCurrent(state,&requested.snapshot,&r->snapshot,AMDGPU_TRANSFER_MAX_BYTES,&capture_bytes,&error);
        if(result){error=AsError(result,error);goto done;}r->snapshot_bytes=capture_bytes;r->bytes=capture_bytes+AMDGPU_ROLLBACK_PREFIX;
        error=AmdgpuWindowsRollbackSave(r,r->bytes,2);
        if(!error)error=AmdgpuWindowsRollbackSave(r,r->bytes,FALSE);goto done;
    }
    if(!wcscmp(argv[1],L"rollback-check")){error=active?Quiescent(argv[4]):ERROR_REVISION_MISMATCH;goto done;}
    if(!wcscmp(argv[1],L"rollback-remove")) {
        if(!active){error=ERROR_REVISION_MISMATCH;goto done;}
        error=AmdgpuWindowsRollbackRead(r->snapshot.transaction,TRUE,&complete,&n);
        if(!error){error=ERROR_REVISION_MISMATCH;goto done;}if(error!=ERROR_FILE_NOT_FOUND)goto done;
        error=Origin(&origin);if(error)goto done;
        if(origin==AMDGPU_TRANSFER_CURRENT) {
            result=AmdgpuWindowsStateOpen(&state,1,&error);if(result){error=AsError(result,error);goto done;}
            result=AmdgpuWindowsStateIdentity(state,generation,&error);
            if(result || memcmp(generation,r->snapshot.source_generation,16)){error=error?error:ERROR_REVISION_MISMATCH;goto done;}
            result=AmdgpuWindowsStateFreeze(state,r->snapshot.transaction,&error);if(result){error=AsError(result,error);goto done;}
        } else if(origin!=AMDGPU_TRANSFER_LEGACY && origin!=AMDGPU_TRANSFER_FRESH){error=ERROR_REVISION_MISMATCH;goto done;}
        error=Source(argv[4],r,origin==AMDGPU_TRANSFER_CURRENT?1:2);goto done;
    }
    AmdgpuStateBootToken(BootId(),boot);
    if(!memcmp(boot,r->snapshot.source_boot,16)){error=ERROR_RETRY;goto done;}
    error=AmdgpuWindowsRollbackRead(r->snapshot.transaction,TRUE,&complete,&n);
    if(!error && (n!=bytes || memcmp(complete,r,bytes))){error=ERROR_REVISION_MISMATCH;goto done;}
    if(error!=0 && error!=ERROR_FILE_NOT_FOUND)goto done;
    {DWORD check=Target(argv[4],r,complete?2:0);if(check){error=check;goto done;}}
    if(enable) {
        if(error)goto done;error=Origin(&origin);if(error)goto done;
        if(origin!=AMDGPU_TRANSFER_LEGACY){error=ERROR_REVISION_MISMATCH;goto done;}
        if(!active){error=Target(argv[4],r,4);goto done;}
        error=Target(argv[4],r,3);
        if(!error)error=AmdgpuWindowsRollbackClear(r,bytes);goto done;
    }
    if(!error)goto done;if(error!=ERROR_FILE_NOT_FOUND)goto done;
    if(!active){error=ERROR_REVISION_MISMATCH;goto done;}
    error=Origin(&origin);if(error)goto done;
    if(origin==AMDGPU_TRANSFER_CURRENT) {
        result=AmdgpuWindowsStateOpen(&state,1,&error);if(result){error=AsError(result,error);goto done;}
        result=AmdgpuWindowsStateIdentity(state,generation,&error);
        if(result || memcmp(generation,r->snapshot.source_generation,16)){error=error?error:ERROR_REVISION_MISMATCH;goto done;}
        result=AmdgpuWindowsStateFreeze(state,r->snapshot.transaction,&error);if(result){error=AsError(result,error);goto done;}
    } else if(origin!=AMDGPU_TRANSFER_LEGACY){error=ERROR_REVISION_MISMATCH;goto done;}
    error=AmdgpuInstallerRollbackRestore(r,bytes);if(error)goto done;
    error=Target(argv[4],r,FALSE);if(error)goto done;
    if(state){result=AmdgpuWindowsStateRetire(state,r->snapshot.transaction,&error);if(result){error=AsError(result,error);goto done;}}
    error=AmdgpuWindowsRollbackSave(r,bytes,TRUE);
done:
    if(complete)HeapFree(GetProcessHeap(),0,complete);if(r)HeapFree(GetProcessHeap(),0,r);
    if(state)AmdgpuWindowsStateClose(state);return error;
}
typedef struct installer_setting {DWORD present,value;} INSTALLER_SETTING;
static BOOL ComparedSetting(const AMDGPU_STATE_SPEC *spec)
{return spec && !spec->path[0] && spec->kind!=AMDGPU_STATE_VOLATILE &&
    spec->type==AMDGPU_STATE_DWORD && !spec->private_read;}
// This observation only vetoes stale installer writes. It is not a migration
// payload and grants no authority to import state. No freeze or SCM write occurs.
static DWORD ReadSettings(PCWSTR device,INSTALLER_SETTING *values)
{
    AMDGPU_TRANSFER_HEADER h={0};AMDGPU_WINDOWS_STATE *state=NULL;UINT i,result;DWORD error;
    error=Device(device,&h,FALSE);if(error)return error;
    error=Origin(&h.origin);if(error)return error;
    if(h.source_schema==AmdgpuStateSchema() && h.origin!=AMDGPU_TRANSFER_CURRENT)return ERROR_INVALID_DATA;
    if(h.origin!=AMDGPU_TRANSFER_FRESH && h.origin!=AMDGPU_TRANSFER_LEGACY &&
       h.origin!=AMDGPU_TRANSFER_CURRENT)return ERROR_INVALID_DATA;
    if(h.origin==AMDGPU_TRANSFER_CURRENT) {
        // The installed generation may belong to an earlier catalog of this schema (an update in place that
        // adds or changes settings): a migration source, read by setting name.
        result=AmdgpuWindowsStateOpenSource(&state,0,&error);if(result)return AsError(result,error);
    }
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);AMDGPU_STATE_VALUE value={0};DWORD data=0;
        values[i].present=values[i].value=0;if(!ComparedSetting(spec))continue;
        if(state)result=AmdgpuWindowsStateQuery(state,spec->path,spec->name,
            spec->kind==AMDGPU_STATE_RECORD?AMDGPU_STATE_READ_RECORD:AMDGPU_STATE_QUERY_OVERRIDE,&value,&data,4);
        else if(h.origin==AMDGPU_TRANSFER_LEGACY)result=AmdgpuWindowsLegacyComparisonValue(spec,&value,&data);
        else result=AMDGPU_STATE_ABSENT;
        if(result==AMDGPU_STATE_ABSENT)continue;
        if(result){error=AsError(result,value.os_error);break;}
        if(value.type!=AMDGPU_STATE_DWORD || value.bytes!=4 ||
           AmdgpuStateValidate(spec,value.type,&data,value.bytes)){error=ERROR_INVALID_DATA;break;}
        values[i].present=1;values[i].value=data;
    }
    if(state)AmdgpuWindowsStateClose(state);return error;
}
typedef struct archive_read {void *data;UINT bytes;} ARCHIVE_READ;
static UINT ArchiveFetch(void *context,UINT source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *out,UINT capacity)
{
    ARCHIVE_READ *a=context;const void *data;UINT i,result;(void)source;
    for(i=0;i<AmdgpuStateCount();i++)if(AmdgpuStateAt(i)==spec)break;
    result=AmdgpuTransferFind(a->data,a->bytes,i,value,&data);
    if(!result){if(value->bytes>capacity)return AMDGPU_STATE_BUFFER;memcpy(out,data,value->bytes);}
    return result;
}
static DWORD Execute(DWORD argc,WCHAR **argv)
{
    AMDGPU_TRANSFER_HEADER h={0},previous;AMDGPU_WINDOWS_STATE *state=NULL;
    void *buffer=NULL,*pending=NULL;DWORD error=0,board=0,pending_bytes=0;UINT bytes=0,result;
    BOOL archive;
    if(argc>1 && (!wcscmp(argv[1],L"rollback-capture") || !wcscmp(argv[1],L"rollback-restore") || !wcscmp(argv[1],L"rollback-enable") || !wcscmp(argv[1],L"rollback-check") || !wcscmp(argv[1],L"rollback-remove")))return Rollback(argc,argv);
    // ServiceMain receives service name followed by this fixed four-argument
    // protocol. All source state is read by this process, never from caller data.
    if(argc!=5 || (!wcscmp(argv[1],L"prepare")?FALSE:
       !wcscmp(argv[1],L"ack")?FALSE:!wcscmp(argv[1],L"archive")?FALSE:TRUE) ||
       !Hex(argv[2],h.transaction) || !Version(argv[3],h.destination_version))return ERROR_INVALID_PARAMETER;
    error=AmdgpuWindowsRollbackRead(NULL,2,&pending,&pending_bytes);
    if(!error){HeapFree(GetProcessHeap(),0,pending);return ERROR_BUSY;}
    if(error!=ERROR_FILE_NOT_FOUND)return error;
    archive=!wcscmp(argv[1],L"archive");
    error=Device(argv[4],&h,FALSE);if(error)return error;
    if(!wcscmp(argv[1],L"ack"))return AmdgpuWindowsAcknowledgeHandoff(h.transaction,h.device_hash,h.destination_version);
    error=AmdgpuWindowsReadHandoff(&pending,&pending_bytes);
    if(!error) {
        result=AmdgpuTransferCheck(pending,pending_bytes,&previous);
        if(result || archive || memcmp(previous.transaction,h.transaction,16) ||
           memcmp(previous.device_hash,h.device_hash,32) || strcmp(previous.destination_version,h.destination_version))error=ERROR_REVISION_MISMATCH;
        HeapFree(GetProcessHeap(),0,pending);return error;
    }
    if(error!=ERROR_FILE_NOT_FOUND)return error;
    error=Origin(&h.origin);if(error)return error;
    if(h.source_schema==AmdgpuStateSchema() && h.origin!=AMDGPU_TRANSFER_CURRENT)return ERROR_INVALID_DATA;
    if(h.origin==AMDGPU_TRANSFER_FRESH) {
        void *removed=NULL;DWORD removed_bytes=0;
        error=AmdgpuWindowsReadRemoval(&removed,&removed_bytes);
        if(!error) {
            AMDGPU_TRANSFER_HEADER old;ARCHIVE_READ read;
            if(AmdgpuTransferCheck(removed,removed_bytes,&old) || memcmp(old.device_hash,h.device_hash,32))error=ERROR_REVISION_MISMATCH;
            else if(archive) {
                // A resumed uninstall after SCM deletion can use only the exact
                // protected final archive it created before that deletion.
                if(memcmp(old.transaction,h.transaction,16) || strcmp(old.destination_version,h.destination_version))error=ERROR_REVISION_MISMATCH;
            } else if(old.origin!=AMDGPU_TRANSFER_CURRENT)error=ERROR_NOT_SUPPORTED;
            else {
                old.origin=AMDGPU_TRANSFER_ARCHIVE;old.board_kind=AMDGPU_BOARD_UNPROVEN;
                ZeroMemory(old.board_record,sizeof(old.board_record));memcpy(old.transaction,h.transaction,16);
                memcpy(old.destination_version,h.destination_version,32);
                AmdgpuStateBootToken(BootId(),old.source_boot);
                buffer=HeapAlloc(GetProcessHeap(),0,AMDGPU_TRANSFER_MAX_BYTES);
                if(!buffer)error=ERROR_NOT_ENOUGH_MEMORY;
                else {
                    read.data=removed;read.bytes=removed_bytes;
                    result=AmdgpuTransferCapture(&old,ArchiveFetch,&read,buffer,AMDGPU_TRANSFER_MAX_BYTES,&bytes);
                    error=AsError(result,0);if(!error)error=AmdgpuWindowsPublishHandoff(buffer,bytes);
                }
            }
            HeapFree(GetProcessHeap(),0,removed);if(buffer)HeapFree(GetProcessHeap(),0,buffer);return error;
        }
        if(error!=ERROR_FILE_NOT_FOUND)return error;
    }
    if(h.origin==AMDGPU_TRANSFER_FRESH)ZeroMemory(h.source_version,sizeof(h.source_version));
    else if(!h.source_version[0])return ERROR_INVALID_DATA;
    h.source_schema=h.origin==AMDGPU_TRANSFER_CURRENT?AmdgpuStateSchema():h.origin==AMDGPU_TRANSFER_LEGACY?1:0;
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);
    // WDK 26100 ntddk.h KUSER_SHARED_DATA.BootId, same reading as the KMD.
    AmdgpuStateBootToken(BootId(),h.source_boot);
    buffer=HeapAlloc(GetProcessHeap(),0,AMDGPU_TRANSFER_MAX_BYTES);if(!buffer)return ERROR_NOT_ENOUGH_MEMORY;
    if(h.origin==AMDGPU_TRANSFER_CURRENT) {
        // The generation of the installed package may belong to an earlier catalog: a migration source. The
        // capture reads it by setting name; the KMD's transaction writes the new generation under this catalog.
        result=AmdgpuWindowsStateOpenSource(&state,1,&error);if(result){error=AsError(result,error);goto done;}
        result=AmdgpuWindowsStateIdentity(state,h.source_generation,&error);if(result){error=AsError(result,error);goto done;}
        result=AmdgpuWindowsStateFreeze(state,h.transaction,&error);if(result){error=AsError(result,error);goto done;}
        if(archive){error=Device(argv[4],&h,TRUE);if(error)goto done;}
        result=AmdgpuWindowsCaptureCurrent(state,&h,buffer,AMDGPU_TRANSFER_MAX_BYTES,&bytes,&error);
    } else {
        if(archive && h.origin!=AMDGPU_TRANSFER_LEGACY){error=ERROR_REVISION_MISMATCH;goto done;}
        if(archive){error=Device(argv[4],&h,TRUE);if(error)goto done;}
        result=AmdgpuWindowsCaptureLegacy(&h,buffer,AMDGPU_TRANSFER_MAX_BYTES,&bytes,&error,&board);
        // Rendering migration may retain UNPROVEN, but inaccessible or corrupt
        // original-board data must not be reported as a successful archive.
        if(board){error=board;goto done;}
    }
    error=AsError(result,error);if(error)goto done;
    error=archive?AmdgpuWindowsArchiveState(buffer,bytes):AmdgpuWindowsPublishHandoff(buffer,bytes);
done:
    // On failure retain freeze, files and old generation for an exact retry.
    if(state)AmdgpuWindowsStateClose(state);if(buffer)HeapFree(GetProcessHeap(),0,buffer);return error;
}
#ifdef AMDGPU_INSTALLER_TEST
DWORD AmdgpuInstallerTestExecute(DWORD argc,WCHAR **argv){return Execute(argc,argv);}
DWORD AmdgpuInstallerTestSettings(PCWSTR device,DWORD *pairs)
{return ReadSettings(device,(INSTALLER_SETTING*)pairs);}
#else
static DWORD WINAPI Control(DWORD control,DWORD event,void *data,void *context)
{(void)control;(void)event;(void)data;(void)context;return ERROR_CALL_NOT_IMPLEMENTED;}
static void WINAPI ServiceMain(DWORD argc,WCHAR **argv)
{
    DWORD error;StatusHandle=RegisterServiceCtrlHandlerExW(ServiceName,Control,NULL);if(!StatusHandle)return;
    ZeroMemory(&ServiceStatus,sizeof(ServiceStatus));ServiceStatus.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    ServiceStatus.dwCurrentState=SERVICE_RUNNING;SetServiceStatus(StatusHandle,&ServiceStatus);
    error=Execute(argc,argv);ServiceStatus.dwCurrentState=SERVICE_STOPPED;
    ServiceStatus.dwWin32ExitCode=error?ERROR_SERVICE_SPECIFIC_ERROR:0;ServiceStatus.dwServiceSpecificExitCode=error;
    SetServiceStatus(StatusHandle,&ServiceStatus);
}
int wmain(int argc,WCHAR **argv)
{
    SC_HANDLE manager=NULL,service=NULL;WCHAR image[32768],command[32768];DWORD error=0,bytes;ULONGLONG until;
    SERVICE_STATUS_PROCESS status;SERVICE_TABLE_ENTRYW table[]={{(LPWSTR)ServiceName,ServiceMain},{NULL,NULL}};
    // Pure metadata only: release verification may run this exact branch on the
    // build PC. It executes before SCM, token, file, registry or device access.
    if(argc==2 && !wcscmp(argv[1],L"--catalog")) {
        printf("{\"abi\":1,\"schema\":%u,\"error\":0,\"catalog_id\":\"%s\",\"count\":%u}\n",
            AmdgpuStateSchema(),AmdgpuStateCatalogId(),AmdgpuStateCount());return 0;
    }
    if(argc==3 && !wcscmp(argv[1],L"--settings-snapshot")) {
        UINT i,count=0;INSTALLER_SETTING *values=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,
            sizeof(*values)*AmdgpuStateCount());
        error=values?ReadSettings(argv[2],values):ERROR_NOT_ENOUGH_MEMORY;
        printf("{\"abi\":1,\"error\":%lu,\"items\":[",error);
        if(!error)for(i=0;i<AmdgpuStateCount();i++) {
            const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);if(!ComparedSetting(spec))continue;
            printf("%s{\"name\":\"%s\",\"present\":%s,\"value\":%lu}",count++?",":"",spec->name,
                values[i].present?"true":"false",values[i].value);
        }
        puts("]}");if(values)HeapFree(GetProcessHeap(),0,values);return (int)error;
    }
    if(argc==2 && !wcscmp(argv[1],L"--service"))return StartServiceCtrlDispatcherW(table)?0:(int)GetLastError();
    if(argc!=5 && argc!=8){fwprintf(stderr,L"usage: amdgpu-state-installer verb transaction product-quad device-instance [inf-hash sys-hash manifest-hash]\n");return ERROR_INVALID_PARAMETER;}
    // These are explicit installer calls only. Tests never execute this program.
    if(!GetModuleFileNameW(NULL,image,32768) || wcschr(image,L'"') ||
       FAILED(StringCchPrintfW(command,32768,L"\"%s\" --service",image)))return ERROR_INVALID_NAME;
    manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT|SC_MANAGER_CREATE_SERVICE);if(!manager){error=GetLastError();goto done;}
    service=CreateServiceW(manager,ServiceName,ServiceName,SERVICE_START|SERVICE_QUERY_STATUS|DELETE,
        SERVICE_WIN32_OWN_PROCESS,SERVICE_DEMAND_START,SERVICE_ERROR_NORMAL,command,NULL,NULL,NULL,L"LocalSystem",NULL);
    if(!service){error=GetLastError();goto done;}
    if(!StartServiceW(service,(DWORD)argc-1,(LPCWSTR*)(argv+1))){error=GetLastError();goto done;}
    until=GetTickCount64()+120000;
    for(;;) {
        if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE*)&status,sizeof(status),&bytes)){error=GetLastError();break;}
        if(status.dwCurrentState==SERVICE_STOPPED){error=status.dwWin32ExitCode==ERROR_SERVICE_SPECIFIC_ERROR?status.dwServiceSpecificExitCode:status.dwWin32ExitCode;break;}
        if(GetTickCount64()>=until){error=ERROR_TIMEOUT;break;}Sleep(100);
    }
done:
    // Mark for deletion; SCM keeps a still-running worker alive until it drains.
    if(service){if(!DeleteService(service) && !error)error=GetLastError();CloseServiceHandle(service);}
    if(manager)CloseServiceHandle(manager);
    wprintf(L"{\"abi\":1,\"error\":%lu}\n",error);return (int)error;
}
#endif
