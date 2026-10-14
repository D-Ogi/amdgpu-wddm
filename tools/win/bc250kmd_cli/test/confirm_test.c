#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "client.h"
static unsigned checks,failures,writes,closes;
static DWORD openError,writeError;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static DWORD WINAPI FakeOpen(AMDGPU_WINDOWS_STATE **state,DWORD writable)
{CHECK(writable==1);*state=openError?NULL:(AMDGPU_WINDOWS_STATE*)1;return openError;}
static DWORD WINAPI FakeWrite(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name,DWORD type,const void* data,DWORD bytes)
{CHECK(state==(AMDGPU_WINDOWS_STATE*)1&&!path[0]&&!strcmp(name,"UnconfirmedStarts"));CHECK(type==AMDGPU_STATE_DWORD&&bytes==sizeof(DWORD)&&*(const DWORD*)data==0);writes++;return writeError;}
static void WINAPI FakeClose(AMDGPU_WINDOWS_STATE *state){CHECK(state==(AMDGPU_WINDOWS_STATE*)1);closes++;}
#define AmdgpuClientOpen FakeOpen
#define AmdgpuClientWrite FakeWrite
#define AmdgpuClientClose FakeClose
#include "confirm_actual.inc"
static void Reset(void){writes=closes=openError=writeError=0;}
int main(void)
{
    Reset();CHECK(Confirm()==0&&writes==1&&closes==1);
    Reset();writeError=ERROR_ACCESS_DENIED;CHECK(Confirm()==1&&writes==1&&closes==1);
    Reset();writeError=ERROR_REGISTRY_IO_FAILED;CHECK(Confirm()==1&&writes==1&&closes==1);
    Reset();openError=ERROR_SERVICE_DOES_NOT_EXIST;CHECK(Confirm()==2&&!writes&&!closes);
    Reset();openError=ERROR_FILE_NOT_FOUND;CHECK(Confirm()==1&&!writes&&!closes);
    Reset();openError=ERROR_ACCESS_DENIED;CHECK(Confirm()==1&&!writes&&!closes);
    printf("CLI confirmation: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
