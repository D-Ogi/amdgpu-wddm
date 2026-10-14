#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "installer_rollback.h"

DWORD AmdgpuRollbackCheck(const void *data,DWORD bytes)
{
    const AMDGPU_ROLLBACK *r=data;DWORD i;BYTE a=0,b=0,c=0;
    if(!r || bytes<AMDGPU_ROLLBACK_PREFIX+sizeof(r->snapshot) || bytes>AMDGPU_ROLLBACK_MAX ||
       r->magic!=AMDGPU_ROLLBACK_MAGIC || r->version!=1 || r->bytes!=bytes ||
       r->snapshot_bytes!=bytes-AMDGPU_ROLLBACK_PREFIX ||
       AmdgpuTransferCheck(&r->snapshot,r->snapshot_bytes,NULL) ||
       r->snapshot.origin!=AMDGPU_TRANSFER_CURRENT)return ERROR_INVALID_DATA;
    for(i=0;i<32;i++){a|=r->inf_hash[i];b|=r->sys_hash[i];c|=r->manifest_hash[i];}
    if(!a || !b || !c || r->source_inf[31] || wcslen(r->source_inf)<8 ||
       wcsncmp(r->source_inf,L"oem",3) || wcspbrk(r->source_inf,L"\\/:") ||
       wcscmp(r->source_inf+wcslen(r->source_inf)-4,L".inf"))return ERROR_INVALID_DATA;
    for(i=3;i<wcslen(r->source_inf)-4;i++)if(r->source_inf[i]<L'0' || r->source_inf[i]>L'9')return ERROR_INVALID_DATA;
    a=0;for(i=0;i<32;i++)a|=r->source_inf_hash[i];return a?0:ERROR_INVALID_DATA;
}
DWORD AmdgpuRollbackConvert(const void *data,DWORD bytes,AMDGPU_ROLLBACK_WRITE write,void *context)
{
    const AMDGPU_ROLLBACK *r=data;DWORD error,i;UINT status;const void *value;AMDGPU_STATE_VALUE v;
    error=AmdgpuRollbackCheck(data,bytes);if(error || !write)return error?error:ERROR_INVALID_PARAMETER;
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);DWORD fallback=0;BOOL present=FALSE;
        value=NULL;ZeroMemory(&v,sizeof(v));
        if(spec->kind==AMDGPU_STATE_VOLATILE) {
            error=write(context,spec,FALSE,NULL,0);if(error)return error;continue;
        }
        status=AmdgpuTransferFind(&r->snapshot,r->snapshot_bytes,i,&v,&value);
        if(status!=AMDGPU_STATE_OK && status!=AMDGPU_STATE_ABSENT)return ERROR_INVALID_DATA;
        if(spec->kind==AMDGPU_STATE_SETTING) {
            if(status==AMDGPU_STATE_OK)present=TRUE;
            else if(spec->factory_present || spec->code_present) {
                fallback=spec->factory_present?spec->factory:spec->code;
                value=&fallback;v.bytes=sizeof(fallback);present=TRUE;
            }
            // The legacy reader treats deletion as off. Reset-to-default was
            // resolved above using the CURRENT package's exact catalog.
            if(spec->legacy_off && present && v.bytes==4 && !*(const DWORD*)value)present=FALSE;
        } else if(spec->kind==AMDGPU_STATE_RECORD && (spec->group || !strcmp(spec->name,"UnconfirmedStarts")) && status==AMDGPU_STATE_OK) {
            // Safety closure and pending/consumed records survive. Diagnostic
            // records stay in the archive; boot confirmations never transfer.
            present=TRUE;
        }
        error=write(context,spec,present,value,present?v.bytes:0);if(error)return error;
    }
    return 0;
}
