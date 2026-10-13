/* Protocol tests use only a fake common client. No registry or GPU access. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "../../../driver/state/client.h"
static unsigned Checks, Failures, Opens, Writes, Clears, Reads, Closes;
static DWORD OpenError, WriteError;
static unsigned ReadStatus;
static char Output[32768];
static size_t Used;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x); } } while(0)
static int Capture(const char *format,...)
{
    va_list args; int n; va_start(args,format);
    n=vsnprintf(Output+Used,sizeof(Output)-Used,format,args); va_end(args);
    if(n>0 && (size_t)n<sizeof(Output)-Used) Used+=(size_t)n;
    return n;
}
DWORD WINAPI AmdgpuClientCount(void) { return 3; }
unsigned int AmdgpuStateSchema(void) { return 2; }
const char *AmdgpuStateCatalogId(void) { return "fixture-catalog"; }
DWORD WINAPI AmdgpuClientLogDirectory(WCHAR *out,DWORD cch)
{ CHECK(cch>=32);wcscpy_s(out,cch,L"Q:\\fixture\\Logs");return OpenError; }
DWORD WINAPI AmdgpuClientSpec(DWORD index,AMDGPU_CLIENT_SPEC *s,DWORD bytes)
{
    CHECK(index<3 && bytes==sizeof(*s)); memset(s,0,sizeof(*s));
    s->abi=1;s->bytes=sizeof(*s);s->kind=index?AMDGPU_STATE_RECORD:AMDGPU_STATE_SETTING;
    s->type=index?AMDGPU_STATE_SZ:AMDGPU_STATE_DWORD;s->max_bytes=index?128:4;
    strcpy_s(s->name,sizeof(s->name),index==0?"Switch":index==1?"Message":"Private");
    if(index)strcpy_s(s->path,sizeof(s->path),"Record");s->private_read=index==2;
    return 0;
}
DWORD WINAPI AmdgpuClientOpen(AMDGPU_WINDOWS_STATE **s,DWORD writable)
{CHECK(writable<=1);++Opens;*s=OpenError?NULL:(AMDGPU_WINDOWS_STATE*)1;return OpenError;}
void WINAPI AmdgpuClientClose(AMDGPU_WINDOWS_STATE *s){if(s)++Closes;}
DWORD WINAPI AmdgpuClientRead(AMDGPU_WINDOWS_STATE *s,const char *path,const char *name,DWORD effective,
    AMDGPU_STATE_VALUE *v,void *data,DWORD bytes)
{
    static const WCHAR message[]=L"quote\"\\\n\x0105";
    CHECK(s==(AMDGPU_WINDOWS_STATE*)1);++Reads;memset(v,0,sizeof(*v));
    v->status=ReadStatus;v->source=effective?AMDGPU_STATE_FACTORY:AMDGPU_STATE_OVERRIDE;
    if(!strcmp(name,"Private")){v->status=AMDGPU_STATE_ACCESS;v->os_error=ERROR_ACCESS_DENIED;return ERROR_ACCESS_DENIED;}
    if(ReadStatus)return ERROR_FILE_NOT_FOUND; /* errors can share this OS code */
    if(!path[0]){DWORD n=42;CHECK(bytes==4);memcpy(data,&n,4);v->type=AMDGPU_STATE_DWORD;v->bytes=4;}
    else {CHECK(bytes>=sizeof(message));memcpy(data,message,sizeof(message));v->type=AMDGPU_STATE_SZ;v->bytes=sizeof(message);}
    return 0;
}
DWORD WINAPI AmdgpuClientWrite(AMDGPU_WINDOWS_STATE *s,const char *p,const char *n,DWORD type,const void *data,DWORD bytes)
{CHECK(s==(AMDGPU_WINDOWS_STATE*)1&&!p[0]&&!strcmp(n,"Switch")&&type==AMDGPU_STATE_DWORD&&bytes==4);CHECK(*(const DWORD*)data==MAXDWORD);++Writes;return WriteError;}
DWORD WINAPI AmdgpuClientClear(AMDGPU_WINDOWS_STATE *s,const char *p,const char *n)
{CHECK(s==(AMDGPU_WINDOWS_STATE*)1&&!p[0]&&!strcmp(n,"Switch"));++Clears;return WriteError;}
#define printf Capture
#include "state_command.h"
#undef printf
static void Reset(void){Opens=Writes=Clears=Reads=Closes=OpenError=WriteError=ReadStatus=0;Used=0;Output[0]=0;}
int main(void)
{
    WCHAR *catalog[]={L"cli",L"state",L"catalog"};
    WCHAR *paths[]={L"cli",L"state",L"paths"};
    WCHAR *snapshot[]={L"cli",L"state",L"snapshot"};
    WCHAR *get[]={L"cli",L"state",L"get",L"Switch"};
    WCHAR *over[]={L"cli",L"state",L"get",L"Switch",L"--override"};
    WCHAR *msg[]={L"cli",L"state",L"get",L"Message",L"Record"};
    WCHAR *set[]={L"cli",L"state",L"set",L"Switch",L"0xffffffff"};
    WCHAR *clear[]={L"cli",L"state",L"clear",L"Switch"};
    WCHAR *bad[]={L"cli",L"state",L"set",L"Switch",L"4294967296"};
    WCHAR *path[]={L"cli",L"state",L"get",L"Switch",L"..\\other"};
    DWORD number;
    Reset();CHECK(StateCommand(3,catalog)==0&&!Opens&&strstr(Output,"\"items\":[")!=NULL);
    CHECK(strstr(Output,"\"schema\":2,\"catalog_id\":\"fixture-catalog\"")!=NULL);
    Reset();CHECK(StateCommand(3,paths)==0&&!Opens&&strstr(Output,"Q:\\\\fixture\\\\Logs")!=NULL);
    Reset();OpenError=ERROR_ACCESS_DENIED;CHECK(StateCommand(3,paths)==1&&!Opens&&strstr(Output,"\"logs\":\"\"")!=NULL);
    Reset();CHECK(StateCommand(3,snapshot)==0&&Reads==4&&Closes==1);CHECK(strstr(Output,"\"override\":")&&strstr(Output,"\"effective\":")&&strstr(Output,"\"status\":\"access\""));
    Reset();CHECK(StateCommand(4,get)==0&&strstr(Output,"\"value\":42")&&strstr(Output,"\"source\":\"factory\""));
    Reset();CHECK(StateCommand(5,over)==0&&strstr(Output,"\"source\":\"override\""));
    Reset();CHECK(StateCommand(5,msg)==0&&strstr(Output,"quote\\\"\\\\\\u000a\\u0105"));
    Reset();CHECK(StateCommand(5,set)==0&&Writes==1&&Closes==1);
    Reset();WriteError=ERROR_REGISTRY_IO_FAILED;CHECK(StateCommand(5,set)==1&&Writes==1);
    Reset();CHECK(StateCommand(4,clear)==0&&Clears==1);
    Reset();CHECK(StateCommand(5,bad)==2&&!Opens&&!Writes);
    Reset();CHECK(StateCommand(5,path)==2&&!Opens&&!Reads);
    Reset();ReadStatus=AMDGPU_STATE_ABSENT;CHECK(StateCommand(4,get)==3&&strstr(Output,"\"status\":\"absent\""));
    Reset();ReadStatus=AMDGPU_STATE_INCOMPLETE;CHECK(StateCommand(4,get)==1&&strstr(Output,"\"status\":\"incomplete\""));
    Reset();OpenError=ERROR_SERVICE_DOES_NOT_EXIST;CHECK(StateCommand(4,get)==1&&strstr(Output,"\"service_installed\":false"));
    Reset();OpenError=ERROR_FILE_NOT_FOUND;CHECK(StateCommand(4,get)==1&&strstr(Output,"\"service_installed\":null"));
    CHECK(!StateParseDword(L"-1",&number)&&!StateParseDword(L"+1",&number)&&!StateParseDword(L" 1",&number));
    CHECK(!StateParseDword(L"0x",&number)&&!StateParseDword(L"0x100000000",&number));
    CHECK(StateParseDword(L"010",&number)&&number==10);
    printf("state command: %u checks, %u failures\n",Checks,Failures);return Failures?1:0;
}
