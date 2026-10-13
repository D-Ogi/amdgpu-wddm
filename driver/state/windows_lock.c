#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include "windows_lock.h"
#ifdef AMDGPU_STATE_ADAPTER_TEST
extern const WCHAR *AmdgpuTestWriterMutexName(void);
extern DWORD AmdgpuTestWriterMutexSecurity(PSECURITY_DESCRIPTOR *out);
extern DWORD AmdgpuTestCheckWriterMutexSecurity(HANDLE mutex);
#endif
static DWORD CheckAcl(HANDLE mutex)
{
#ifdef AMDGPU_STATE_ADAPTER_TEST
    return AmdgpuTestCheckWriterMutexSecurity(mutex);
#else
    PSECURITY_DESCRIPTOR sd=NULL;PACL acl=NULL;DWORD error,i;
    BYTE system[SECURITY_MAX_SID_SIZE],admin[SECURITY_MAX_SID_SIZE];
    DWORD system_bytes=sizeof(system),admin_bytes=sizeof(admin);
    SECURITY_DESCRIPTOR_CONTROL control;DWORD revision;
    error=GetSecurityInfo(mutex,SE_KERNEL_OBJECT,DACL_SECURITY_INFORMATION,NULL,NULL,&acl,NULL,&sd);
    if(error) return error;
    if(!CreateWellKnownSid(WinLocalSystemSid,NULL,system,&system_bytes) ||
       !CreateWellKnownSid(WinBuiltinAdministratorsSid,NULL,admin,&admin_bytes) ||
       !GetSecurityDescriptorControl(sd,&control,&revision) || !(control&SE_DACL_PROTECTED) ||
       !acl || acl->AceCount!=2) {error=ERROR_INVALID_SECURITY_DESCR;goto done;}
    for(i=0;i<2;i++) {
        ACCESS_ALLOWED_ACE *ace=NULL;PSID sid=i?admin:system;
        if(!GetAce(acl,i,(void**)&ace) || ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE ||
           ace->Header.AceFlags || ace->Mask!=MUTEX_ALL_ACCESS ||
           ace->Header.AceSize!=FIELD_OFFSET(ACCESS_ALLOWED_ACE,SidStart)+GetLengthSid(sid) ||
           !EqualSid(&ace->SidStart,sid)) {error=ERROR_INVALID_SECURITY_DESCR;goto done;}
    }
done:
    LocalFree(sd);return error;
#endif
}
DWORD AmdgpuStateWriterLock(HANDLE *out)
{
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,FALSE};DWORD error,wait;HANDLE mutex;
    const WCHAR *name=L"Global\\amdgpu-wddm-StateV2-Writer";
    if(!out) return ERROR_INVALID_PARAMETER;*out=NULL;
#ifdef AMDGPU_STATE_ADAPTER_TEST
    name=AmdgpuTestWriterMutexName();
#endif
    // An unprivileged name collision is an error, never an accepted foreign lock.
#ifdef AMDGPU_STATE_ADAPTER_TEST
    error=AmdgpuTestWriterMutexSecurity(&sa.lpSecurityDescriptor);
    if(error)return error;
#else
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;0x1f0001;;;SY)(A;;0x1f0001;;;BA)",SDDL_REVISION_1,&sa.lpSecurityDescriptor,NULL))
        return GetLastError();
#endif
    mutex=CreateMutexExW(&sa,name,0,SYNCHRONIZE|MUTEX_MODIFY_STATE|READ_CONTROL);
    error=mutex?ERROR_SUCCESS:GetLastError();LocalFree(sa.lpSecurityDescriptor);
    if(error) return error;
    error=CheckAcl(mutex);
    if(error) {CloseHandle(mutex);return error;}
    wait=WaitForSingleObject(mutex,30000);
    if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) {
        error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();CloseHandle(mutex);return error;
    }
    // The persistent Frozen/Active checks still run after an abandoned owner.
    *out=mutex;return ERROR_SUCCESS;
}
void AmdgpuStateWriterUnlock(HANDLE mutex)
{ if(mutex) {ReleaseMutex(mutex);CloseHandle(mutex);} }
