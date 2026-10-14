#include <ntifs.h>
#include <ntstrsafe.h>
#include "state_transaction.h"
#include "../state/transfer.h"

#define STATE_ALREADY_APPLIED 0x10000u
typedef struct state_transaction {
    HANDLE shared,root,generations,generation,data,metadata;
    WCHAR transaction[33],previous[33];
    NTSTATUS error;
} STATE_TRANSACTION;
static NTSTATUS SecuredChild(HANDLE parent,PCWSTR path,BOOLEAN privateRead,PHANDLE key)
{
    SECURITY_DESCRIPTOR sd;union { ULONG alignment;UCHAR bytes[128]; } storage;
    PACL acl=(PACL)storage.bytes;OBJECT_ATTRIBUTES oa;UNICODE_STRING name;NTSTATUS status;
    static const ULONG systemSid[]={0x00000101u,0x05000000u,18u};
    static const ULONG adminSid[]={0x00000201u,0x05000000u,32u,544u};
    static const ULONG usersSid[]={0x00000201u,0x05000000u,32u,545u};
    RtlZeroMemory(&storage,sizeof(storage));
    status=RtlCreateSecurityDescriptor(&sd,SECURITY_DESCRIPTOR_REVISION);
    if(NT_SUCCESS(status)) status=RtlCreateAcl(acl,sizeof(storage.bytes),ACL_REVISION);
    if(NT_SUCCESS(status)) status=RtlAddAccessAllowedAceEx(acl,ACL_REVISION,CONTAINER_INHERIT_ACE,KEY_ALL_ACCESS,(PSID)systemSid);
    if(NT_SUCCESS(status)) status=RtlAddAccessAllowedAceEx(acl,ACL_REVISION,CONTAINER_INHERIT_ACE,
                                                         privateRead?KEY_READ:KEY_ALL_ACCESS,(PSID)adminSid);
    if(NT_SUCCESS(status) && !privateRead)
        status=RtlAddAccessAllowedAceEx(acl,ACL_REVISION,CONTAINER_INHERIT_ACE,KEY_READ,(PSID)usersSid);
    if(NT_SUCCESS(status)) status=RtlSetDaclSecurityDescriptor(&sd,TRUE,acl,FALSE);
    if(NT_SUCCESS(status)) status=RtlSetOwnerSecurityDescriptor(&sd,(PSID)systemSid,FALSE);
    if(!NT_SUCCESS(status)) return status;
    sd.Control|=SE_DACL_PROTECTED;
    RtlInitUnicodeString(&name,path);
    InitializeObjectAttributes(&oa,&name,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,parent,&sd);
    status=ZwCreateKey(key,KEY_READ|KEY_WRITE|WRITE_DAC|WRITE_OWNER,&oa,0,NULL,REG_OPTION_NON_VOLATILE,NULL);
    if(NT_SUCCESS(status)) {
        status=ZwSetSecurityObject(*key,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|
                                  PROTECTED_DACL_SECURITY_INFORMATION,&sd);
        if(!NT_SUCCESS(status)) { ZwClose(*key);*key=NULL; }
    }
    return status;
}
static NTSTATUS PrivateChild(HANDLE parent,PCWSTR path,PHANDLE key)
{ return SecuredChild(parent,path,TRUE,key); }
static NTSTATUS Child(HANDLE root,PCWSTR path,BOOLEAN create,PHANDLE key)
{
    UNICODE_STRING name;OBJECT_ATTRIBUTES oa;
    RtlInitUnicodeString(&name,path);
    InitializeObjectAttributes(&oa,&name,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,root,NULL);
    return create?SecuredChild(root,path,FALSE,key):
                  ZwOpenKey(key,KEY_READ|KEY_WRITE,&oa);
}
static NTSTATUS Put(HANDLE key,PCWSTR name,ULONG type,const void *data,ULONG bytes)
{
    UNICODE_STRING value;RtlInitUnicodeString(&value,name);
    return ZwSetValueKey(key,&value,0,type,(PVOID)data,bytes);
}
static NTSTATUS Remove(HANDLE key,PCWSTR name)
{
    UNICODE_STRING value;NTSTATUS status;RtlInitUnicodeString(&value,name);
    status=ZwDeleteValueKey(key,&value);
    return status==STATUS_OBJECT_NAME_NOT_FOUND?STATUS_SUCCESS:status;
}
static NTSTATUS Get(HANDLE key,PCWSTR name,ULONG type,PVOID data,ULONG bytes)
{
    union { ULONG alignment;UCHAR bytes[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+sizeof(AMDGPU_TRANSFER_HEADER)]; } buffer;
    PKEY_VALUE_PARTIAL_INFORMATION info=(PKEY_VALUE_PARTIAL_INFORMATION)buffer.bytes;
    UNICODE_STRING value;ULONG needed=0;NTSTATUS status;
    if(bytes>sizeof(AMDGPU_TRANSFER_HEADER)) return STATUS_INVALID_PARAMETER;
    RtlInitUnicodeString(&value,name);
    status=ZwQueryValueKey(key,&value,KeyValuePartialInformation,info,sizeof(buffer.bytes),&needed);
    if(!NT_SUCCESS(status)) return status;
    if(needed<(ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data) || info->Type!=type ||
       info->DataLength!=bytes || info->DataLength>needed-(ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data))
        return STATUS_OBJECT_TYPE_MISMATCH;
    RtlCopyMemory(data,info->Data,bytes);return STATUS_SUCCESS;
}
static void Hex(const unsigned char *id,WCHAR *out)
{
    static const WCHAR digits[]=L"0123456789abcdef";ULONG i;
    for(i=0;i<16;i++) { out[2*i]=digits[id[i]>>4];out[2*i+1]=digits[id[i]&15]; }out[32]=0;
}
static void Wide(const char *in,WCHAR *out)
{ while(*in) *out++=(UCHAR)*in++;*out=0; }
static NTSTATUS Active(STATE_TRANSACTION *t,BOOLEAN *same)
{
    WCHAR active[33];NTSTATUS status=Get(t->root,L"Active",REG_SZ,active,sizeof(active));
    *same=FALSE;
    if(status==STATUS_OBJECT_NAME_NOT_FOUND && !t->previous[0]) return STATUS_SUCCESS;
    if(!NT_SUCCESS(status)) return status;
    if(active[32]) return STATUS_DATA_ERROR;
    *same=wcscmp(active,t->transaction)==0;
    if(*same) return STATUS_SUCCESS;
    return t->previous[0] && !wcscmp(active,t->previous)?STATUS_SUCCESS:STATUS_REVISION_MISMATCH;
}
static NTSTATUS Thaw(STATE_TRANSACTION *t)
{
    ULONG frozen=0;WCHAR owner[33];NTSTATUS status=Get(t->root,L"Frozen",REG_DWORD,&frozen,4);
    if(status!=STATUS_OBJECT_NAME_NOT_FOUND && !NT_SUCCESS(status)) return status;
    if(NT_SUCCESS(status) && frozen) {
        if(frozen!=1) return STATUS_DATA_ERROR;
        status=Get(t->root,L"FrozenBy",REG_SZ,owner,sizeof(owner));
        if(!NT_SUCCESS(status)) return status;
        if(owner[32] || wcscmp(owner,t->transaction)) return STATUS_REVISION_MISMATCH;
    }
    frozen=0;status=Put(t->root,L"Frozen",REG_DWORD,&frozen,4);
    if(NT_SUCCESS(status)) status=ZwFlushKey(t->root);
    return status;
}
static NTSTATUS Frozen(STATE_TRANSACTION *t)
{
    ULONG frozen=0;WCHAR owner[33];NTSTATUS status=Get(t->root,L"Frozen",REG_DWORD,&frozen,4);
    if(!NT_SUCCESS(status))return status;
    if(frozen!=1)return STATUS_DEVICE_CONFIGURATION_ERROR;
    status=Get(t->root,L"FrozenBy",REG_SZ,owner,sizeof(owner));if(!NT_SUCCESS(status))return status;
    return owner[32] || wcscmp(owner,t->transaction)?STATUS_REVISION_MISMATCH:STATUS_SUCCESS;
}
static NTSTATUS Begin(STATE_TRANSACTION *t,const AMDGPU_TRANSFER_HEADER *h,BOOLEAN *already)
{
    NTSTATUS status;BOOLEAN same;ULONG schema=0;AMDGPU_TRANSFER_HEADER old;
    WCHAR catalog[65];
    *already=FALSE;Hex(h->transaction,t->transaction);
    if(h->origin==AMDGPU_TRANSFER_CURRENT) Hex(h->source_generation,t->previous);
    status=Child(t->shared,L"StateV2",TRUE,&t->root);if(!NT_SUCCESS(status)) return status;
    status=Active(t,&same);if(!NT_SUCCESS(status)) return status;
    if(!same && h->origin==AMDGPU_TRANSFER_CURRENT) {
        status=Frozen(t);if(!NT_SUCCESS(status))return status;
    }
    status=Child(t->root,L"Generations",TRUE,&t->generations);if(!NT_SUCCESS(status)) return status;
    status=Child(t->generations,t->transaction,TRUE,&t->generation);if(!NT_SUCCESS(status)) return status;
    status=PrivateChild(t->generation,L"Migration",&t->metadata);if(!NT_SUCCESS(status)) return status;
    status=Get(t->metadata,L"Transfer",REG_BINARY,&old,sizeof(old));
    if(NT_SUCCESS(status)) {
        if(RtlCompareMemory(&old,h,sizeof(old))!=sizeof(old)) return STATUS_REVISION_MISMATCH;
    } else if(status!=STATUS_OBJECT_NAME_NOT_FOUND || same) return status;
    if(same) {
        // Preserve changes made after the first successful apply, including a
        // cleared override. Only force the pointer durable after a lost flush.
        status=Get(t->generation,L"Ready",REG_DWORD,&schema,sizeof(schema));
        if(!NT_SUCCESS(status) || schema!=AmdgpuStateSchema()) return STATUS_DEVICE_CONFIGURATION_ERROR;
        status=ZwFlushKey(t->root);
        if(NT_SUCCESS(status)) status=Thaw(t);
        if(NT_SUCCESS(status)) *already=TRUE;
        return status;
    }
    status=Remove(t->generation,L"Ready");if(!NT_SUCCESS(status)) return status;
    status=ZwFlushKey(t->generation);if(!NT_SUCCESS(status)) return status;
    status=Put(t->metadata,L"Transfer",REG_BINARY,h,sizeof(*h));if(!NT_SUCCESS(status)) return status;
    Wide(AmdgpuStateCatalogId(),catalog);
    status=Put(t->generation,L"CatalogId",REG_SZ,catalog,sizeof(catalog));if(!NT_SUCCESS(status)) return status;
    status=Child(t->generation,L"Data",TRUE,&t->data);
    return status;
}
static unsigned int Store(void *context,unsigned int op,const AMDGPU_TRANSFER_HEADER *h,
    const AMDGPU_STATE_SPEC *spec,const AMDGPU_STATE_VALUE *value,const void *data)
{
    STATE_TRANSACTION *t=(STATE_TRANSACTION*)context;NTSTATUS status=STATUS_SUCCESS;ULONG number;
    if(op==AMDGPU_TRANSFER_BEGIN) {
        BOOLEAN already=FALSE;status=Begin(t,h,&already);
        if(NT_SUCCESS(status) && already) return STATE_ALREADY_APPLIED;
    } else if(op==AMDGPU_TRANSFER_PUT) {
        WCHAR path[128],name[128];HANDLE key=NULL;
        if(strlen(spec->path)>=128 || strlen(spec->name)>=128) status=STATUS_INVALID_PARAMETER;
        else {
            Wide(spec->path,path);Wide(spec->name,name);
            status=spec->private_read?PrivateChild(t->data,path,&key):Child(t->data,path,TRUE,&key);
            if(NT_SUCCESS(status)) {
                NTSTATUS closed;
                status=value->status==AMDGPU_STATE_ABSENT?Remove(key,name):Put(key,name,value->type,data,value->bytes);
                closed=ZwClose(key);
                if(NT_SUCCESS(status) && !NT_SUCCESS(closed)) status=closed;
            }
        }
    } else if(op==AMDGPU_TRANSFER_FLUSH_DATA) status=ZwFlushKey(t->data);
    else if(op==AMDGPU_TRANSFER_READY) {
        number=AmdgpuStateSchema();status=Put(t->generation,L"Ready",REG_DWORD,&number,sizeof(number));
    } else if(op==AMDGPU_TRANSFER_FLUSH_READY) status=ZwFlushKey(t->generation);
    else if(op==AMDGPU_TRANSFER_ACTIVATE) {
        BOOLEAN same=FALSE;status=Active(t,&same);
        if(NT_SUCCESS(status) && same) status=STATUS_REVISION_MISMATCH;
        if(NT_SUCCESS(status) && h->origin==AMDGPU_TRANSFER_CURRENT)status=Frozen(t);
        if(NT_SUCCESS(status)) status=Put(t->root,L"Active",REG_SZ,t->transaction,sizeof(t->transaction));
    } else if(op==AMDGPU_TRANSFER_FLUSH_ACTIVE) {
        status=ZwFlushKey(t->root);
        if(NT_SUCCESS(status)) status=Thaw(t);
    } else if(op==AMDGPU_TRANSFER_RELEASE) {
        // Keep the first failing close, but close every handle on every path.
        HANDLE *handles[]={&t->data,&t->metadata,&t->generation,&t->generations,&t->root};ULONG i;
        for(i=0;i<RTL_NUMBER_OF(handles);i++) if(*handles[i]) {
            NTSTATUS closed=ZwClose(*handles[i]);*handles[i]=NULL;
            if(NT_SUCCESS(status) && !NT_SUCCESS(closed)) status=closed;
        }
    } else status=STATUS_INVALID_PARAMETER;
    if(!NT_SUCCESS(status) && NT_SUCCESS(t->error)) t->error=status;
    return NT_SUCCESS(status)?AMDGPU_STATE_OK:AMDGPU_STATE_IO;
}
NTSTATUS DriverStateCommitTransfer(HANDLE shared,const void *buffer,ULONG bytes)
{
    STATE_TRANSACTION transaction;unsigned int groups,index,result;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !PsIsSystemThread(PsGetCurrentThread()) || !shared)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&transaction,sizeof(transaction));transaction.shared=shared;
    result=AmdgpuTransferApply(buffer,bytes,Store,&transaction,&groups,&index);
    if(!NT_SUCCESS(transaction.error)) return transaction.error;
    return result==AMDGPU_STATE_OK || result==STATE_ALREADY_APPLIED?STATUS_SUCCESS:STATUS_DEVICE_CONFIGURATION_ERROR;
}
NTSTATUS DriverStateFinishAccepted(HANDLE shared,const AMDGPU_TRANSFER_HEADER *receipt)
{
    STATE_TRANSACTION t;BOOLEAN same=FALSE,already=FALSE;NTSTATUS status;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !PsIsSystemThread(PsGetCurrentThread()) || !shared || !receipt)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&t,sizeof(t));t.shared=shared;Hex(receipt->transaction,t.transaction);
    status=Child(shared,L"StateV2",FALSE,&t.root);
    if(NT_SUCCESS(status))status=Active(&t,&same);
    if(NT_SUCCESS(status) && !same)status=STATUS_REVISION_MISMATCH;
    if(t.root){NTSTATUS closed=ZwClose(t.root);t.root=NULL;if(NT_SUCCESS(status))status=closed;}
    if(NT_SUCCESS(status))status=Begin(&t,receipt,&already);
    if(NT_SUCCESS(status) && !already)status=STATUS_REVISION_MISMATCH;
    (void)Store(&t,AMDGPU_TRANSFER_RELEASE,receipt,NULL,NULL,NULL);
    return NT_SUCCESS(status) && !NT_SUCCESS(t.error)?t.error:status;
}
