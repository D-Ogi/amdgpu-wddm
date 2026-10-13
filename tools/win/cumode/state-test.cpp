// Production state operations against a fake native boundary. No UI or GPU calls.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "cumode-state.h"
static DWORD data[3], errorOpen, errorRead, statusRead, errorWrite, rollbackFailure;
static bool present[3], locked;
static int writes, closes, checks, failed, failAt, reads, wrongAt;
static void Check(bool ok, const char* name) { ++checks; if (!ok) { ++failed; printf("FAIL %s\n", name); } }
static int Index(const char* name) { return !strcmp(name,"CuMode") ? 0 : !strcmp(name,"CuDisableWgp") ? 1 : 2; }
static DWORD Read(const char* name, DWORD effective, AMDGPU_STATE_VALUE* value, void* out, DWORD bytes)
{
    ++reads; memset(value,0,sizeof(*value));
    if (statusRead) { value->status=statusRead; return errorRead; }
    int i=Index(name);
    if (!present[i] && !effective) { value->status=AMDGPU_STATE_ABSENT; return ERROR_FILE_NOT_FOUND; }
    value->type=AMDGPU_STATE_DWORD; value->bytes=sizeof(DWORD);
    if(bytes<sizeof(DWORD))return ERROR_INSUFFICIENT_BUFFER;
    *(DWORD*)out=present[i]?data[i]:24;
    if(reads==wrongAt)*(DWORD*)out=999;
    return 0;
}
extern "C" DWORD WINAPI AmdgpuClientOpen(AMDGPU_WINDOWS_STATE** out,DWORD){*out=(AMDGPU_WINDOWS_STATE*)1;return errorOpen;}
extern "C" void WINAPI AmdgpuClientClose(AMDGPU_WINDOWS_STATE*){++closes;}
extern "C" DWORD WINAPI AmdgpuClientRead(AMDGPU_WINDOWS_STATE*,const char*,const char* name,DWORD effective,AMDGPU_STATE_VALUE* value,void* out,DWORD bytes){return Read(name,effective,value,out,bytes);}
extern "C" DWORD WINAPI AmdgpuClientBeginBatch(AMDGPU_CLIENT_BATCH** out){if(errorOpen)return errorOpen;Check(!locked,"one batch");locked=true;*out=(AMDGPU_CLIENT_BATCH*)1;return 0;}
extern "C" DWORD WINAPI AmdgpuClientEndBatch(AMDGPU_CLIENT_BATCH*){Check(locked,"end owns lock");locked=false;++closes;return 0;}
extern "C" DWORD WINAPI AmdgpuClientBatchRead(AMDGPU_CLIENT_BATCH*,const char*,const char* name,DWORD effective,AMDGPU_STATE_VALUE* value,void* out,DWORD bytes){Check(locked,"read lock");return Read(name,effective,value,out,bytes);}
static DWORD Write(const char* name,bool exists,DWORD value)
{
    Check(locked,"write lock");++writes;int i=Index(name);present[i]=exists;data[i]=value;
    if(writes==failAt)return errorWrite;
    if(failAt&&writes>failAt&&rollbackFailure)return rollbackFailure;
    return 0;
}
extern "C" DWORD WINAPI AmdgpuClientBatchWrite(AMDGPU_CLIENT_BATCH*,const char*,const char* name,DWORD type,const void* value,DWORD bytes){Check(type==4&&bytes==4,"DWORD write");return Write(name,true,*(const DWORD*)value);}
extern "C" DWORD WINAPI AmdgpuClientBatchClear(AMDGPU_CLIENT_BATCH*,const char*,const char* name){return Write(name,false,0);}
static void Reset(){memset(data,0,sizeof(data));memset(present,0,sizeof(present));errorOpen=errorRead=statusRead=errorWrite=rollbackFailure=0;writes=closes=failAt=reads=wrongAt=0;locked=false;}
int main()
{
    DWORD value,rollback;bool exists;
    Reset();Check(ReadCuStateDword("CuMode",true,&value,&exists)==0&&exists&&value==24,"effective native default");
    Reset();Check(ReadCuStateDword("CuModeConfirmed",false,&value,&exists)==0&&!exists,"absent record");
    for(DWORD status : {3u,5u,6u,7u,8u}) { Reset();statusRead=status;errorRead=2;Check(ReadCuStateDword("CuMode",true,&value,&exists)==2&&!exists,"error2 is not absence"); }
    for(DWORD error : {2u,5u,1060u}){Reset();errorOpen=error;Check(ReadCuStateDword("CuMode",true,&value,&exists)==error,"open error preserved");}
    Reset();present[2]=true;data[2]=40;Check(WriteCuState(40,true,16,&rollback)==0&&present[0]&&data[0]==40&&present[1]&&data[1]==16&&!present[2]&&!locked,"write mask and clear old confirmation");
    Reset();present[1]=true;data[1]=16;Check(WriteCuState(24,false,0,&rollback)==0&&!present[1]&&!present[2],"clear mask returns native default");
    Reset();statusRead=8;errorRead=2;Check(WriteCuState(40,false,0,&rollback)==2&&writes==0&&!locked,"bad backup stops all writes");
    for(int at=1;at<=3;++at){Reset();present[0]=true;data[0]=24;present[2]=true;data[2]=40;failAt=at;errorWrite=1016;Check(WriteCuState(40,true,16,&rollback)==1016&&rollback==0&&present[0]&&data[0]==24&&!present[1]&&present[2]&&data[2]==40&&!locked,"flush failure rolls back exact overrides");}
    Reset();wrongAt=4;Check(WriteCuState(40,false,0,&rollback)==ERROR_INVALID_DATA&&!present[0]&&!locked,"readback mismatch rolls back");
    Reset();failAt=2;errorWrite=1016;rollbackFailure=5;Check(WriteCuState(40,true,16,&rollback)==1016&&rollback==5&&!locked,"rollback error visible");
    printf("CU shared state: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
