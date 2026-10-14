#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <strsafe.h>
#include "windows_migration.h"
#include "../kmd/board_memory_format.h"

typedef struct legacy_capture { HKEY parameters; DWORD error; } LEGACY_CAPTURE;
#ifdef AMDGPU_STATE_ADAPTER_TEST
extern DWORD AmdgpuTestLegacyOpen(BOOL board,HKEY *key);
#endif
static DWORD LegacyOpen(BOOL board,HKEY *key)
{
#ifdef AMDGPU_STATE_ADAPTER_TEST
    return AmdgpuTestLegacyOpen(board,key);
#else
    const WCHAR *path=board?L"SYSTEM\\CurrentControlSet\\Control\\amdgpu-wddm-board-memory":
                            L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters";
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE,path,0,KEY_READ|KEY_WOW64_64KEY,key);
#endif
}
static unsigned int Result(DWORD error)
{
    if(error==ERROR_SUCCESS) return AMDGPU_STATE_OK;
    if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return AMDGPU_STATE_ABSENT;
    if(error==ERROR_ACCESS_DENIED) return AMDGPU_STATE_ACCESS;
    if(error==ERROR_MORE_DATA) return AMDGPU_STATE_BUFFER;
    return AMDGPU_STATE_IO;
}
static void Wide(const char *s,WCHAR *w)
{ while(*s) *w++=(unsigned char)*s++; *w=0; }
static unsigned int Raw(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *data,unsigned int capacity)
{
    LEGACY_CAPTURE *capture=(LEGACY_CAPTURE*)context;HKEY key=NULL;
    WCHAR path[128],name[128];DWORD type=0,bytes=capacity,error;
    (void)source;
    if(!capture->parameters) return AMDGPU_STATE_ABSENT;
    if(strlen(spec->path)>=128 || strlen(spec->name)>=128) return AMDGPU_STATE_SCHEMA;
    Wide(spec->path,path);Wide(spec->name,name);
    error=RegOpenKeyExW(capture->parameters,path,0,KEY_QUERY_VALUE,&key);
    if(error==ERROR_SUCCESS) {
        error=RegQueryValueExW(key,name,NULL,&type,(BYTE*)data,&bytes);RegCloseKey(key);
    }
    value->type=type;value->bytes=bytes;value->os_error=error;capture->error=error;
    return Result(error);
}
unsigned int AmdgpuWindowsLegacyComparisonValue(const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,DWORD *data)
{
    LEGACY_CAPTURE capture={0};DWORD error;unsigned int result;
    if(!spec || !value || !data || spec!=AmdgpuStateFind(spec->path,spec->name) ||
       spec->path[0] || spec->kind==AMDGPU_STATE_VOLATILE || spec->type!=AMDGPU_STATE_DWORD ||
       spec->private_read)return AMDGPU_STATE_INVALID_ARGUMENT;
    ZeroMemory(value,sizeof(*value));*data=0;
    error=LegacyOpen(FALSE,&capture.parameters);value->os_error=error;
    if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return AMDGPU_STATE_INCOMPLETE;
    if(error)return Result(error);
    result=AmdgpuStateQuery(spec,spec->kind==AMDGPU_STATE_RECORD?AMDGPU_STATE_READ_RECORD:AMDGPU_STATE_QUERY_OVERRIDE,
        Raw,&capture,value,data,sizeof(*data));
    RegCloseKey(capture.parameters);return result;
}
static BOOL BoardSecurity(HKEY key,DWORD *error)
{
    PSECURITY_DESCRIPTOR sd=NULL;PSID owner=NULL;PACL acl=NULL;
    SECURITY_DESCRIPTOR_CONTROL control;DWORD revision,i;BOOL valid=FALSE;
    BYTE sys[SECURITY_MAX_SID_SIZE],admin[SECURITY_MAX_SID_SIZE];DWORD sysbytes=sizeof(sys),adminbytes=sizeof(admin);
    *error=GetSecurityInfo(key,SE_REGISTRY_KEY,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
                          &owner,NULL,&acl,NULL,&sd);
    if(*error) return FALSE;
    if(!CreateWellKnownSid(WinLocalSystemSid,NULL,sys,&sysbytes) ||
       !CreateWellKnownSid(WinBuiltinAdministratorsSid,NULL,admin,&adminbytes) ||
       !GetSecurityDescriptorControl(sd,&control,&revision) || !owner || !EqualSid(owner,sys) ||
       !(control&SE_DACL_PROTECTED) || !acl || acl->AceCount!=2) goto done;
    for(i=0;i<2;i++) {
        ACCESS_ALLOWED_ACE *ace=NULL;PSID expected=i?admin:sys;DWORD mask=i?KEY_READ:KEY_ALL_ACCESS;
        if(!GetAce(acl,i,(void**)&ace) || ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags ||
           ace->Mask!=mask || ace->Header.AceSize!=FIELD_OFFSET(ACCESS_ALLOWED_ACE,SidStart)+GetLengthSid(expected) ||
           !EqualSid(&ace->SidStart,expected)) goto done;
    }
    valid=TRUE;
done:
    if(!valid) *error=ERROR_INVALID_SECURITY_DESCR;
    LocalFree(sd);return valid;
}
static void Board(AMDGPU_TRANSFER_HEADER *h,DWORD *error)
{
    HKEY key=NULL;DWORD type=0,bytes=sizeof(BOARD_STORE_RECORD),i;
    BOARD_STORE_RECORD record;
    h->board_kind=AMDGPU_BOARD_UNPROVEN;ZeroMemory(h->board_record,sizeof(h->board_record));
    *error=LegacyOpen(TRUE,&key);
    if(*error==ERROR_FILE_NOT_FOUND || *error==ERROR_PATH_NOT_FOUND) {
        // This proves absence of the old store only. The SYSTEM importer still
        // checks every new-store file and refuses an incomplete existing pair.
        h->board_kind=AMDGPU_BOARD_FRESH;*error=ERROR_SUCCESS;return;
    }
    if(*error!=ERROR_SUCCESS) return;
    if(!BoardSecurity(key,error)) { RegCloseKey(key);return; }
    *error=RegQueryValueExW(key,L"Backup",NULL,&type,(BYTE*)&record,&bytes);RegCloseKey(key);
    if(*error!=ERROR_SUCCESS) return;
    if(type!=REG_BINARY || bytes!=sizeof(record) || record.Magic!=BOARD_STORE_MAGIC ||
       record.Version!=BOARD_STORE_VERSION || record.Bytes!=sizeof(record) || record.Pending>1 ||
       record.Checksum!=StoreChecksum(&record)) { *error=ERROR_INVALID_DATA;return; }
    for(i=0;i<4;i++) if(record.Reserved[i]) { *error=ERROR_INVALID_DATA;return; }
    // Mapping, BIOS, machine identity and the UMA block are checked again against
    // the actual device by BoardMemoryStoreSeed. Export does no hardware access.
    memcpy(h->board_record,&record,sizeof(record));h->board_kind=AMDGPU_BOARD_LEGACY;
}
unsigned int AmdgpuWindowsCaptureLegacy(const AMDGPU_TRANSFER_HEADER *metadata,
    void *buffer,unsigned int capacity,unsigned int *bytes,DWORD *error,DWORD *board_error)
{
    AMDGPU_TRANSFER_HEADER h;LEGACY_CAPTURE capture={0};unsigned int result;
    if(!metadata || !bytes || !error || !board_error) return AMDGPU_STATE_INVALID_ARGUMENT;
    *bytes=0;*error=*board_error=0;
    if(metadata->origin!=AMDGPU_TRANSFER_LEGACY && metadata->origin!=AMDGPU_TRANSFER_FRESH) return AMDGPU_STATE_INVALID_ARGUMENT;
    *error=LegacyOpen(FALSE,&capture.parameters);
    if(*error!=ERROR_SUCCESS && *error!=ERROR_FILE_NOT_FOUND && *error!=ERROR_PATH_NOT_FOUND) return Result(*error);
    if(metadata->origin==AMDGPU_TRANSFER_LEGACY && !capture.parameters) return AMDGPU_STATE_INCOMPLETE;
    if(metadata->origin==AMDGPU_TRANSFER_FRESH && capture.parameters) { RegCloseKey(capture.parameters);return AMDGPU_STATE_SCHEMA; }
    h=*metadata;Board(&h,board_error);
    result=AmdgpuTransferCapture(&h,Raw,&capture,buffer,capacity,bytes);
    *error=result?capture.error:0;
    if(capture.parameters) RegCloseKey(capture.parameters);
    return result;
}
