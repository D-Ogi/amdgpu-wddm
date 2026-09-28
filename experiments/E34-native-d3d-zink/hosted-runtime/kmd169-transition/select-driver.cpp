// Read-only SetupAPI selection probe. No DIF installation or device state change.
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <cwchar>
#include <vector>
struct DeviceSet {
    HDEVINFO h=INVALID_HANDLE_VALUE;
    ~DeviceSet(){if(h!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(h);}
};
static int fail(const char* operation){
    fprintf(stderr,"%s failed: %lu\n",operation,GetLastError());return 1;
}
int wmain(int argc,wchar_t** argv){
    if(argc==2 && wcscmp(argv[1],L"--help")==0){
        puts("select-driver --inspect INSTANCE ABSOLUTE_INF; read-only compatible-node selection");return 0;
    }
    if(argc!=4 || wcscmp(argv[1],L"--inspect")!=0)return 2;
    const wchar_t prefix[]=L"PCI\\VEN_1002&DEV_13FE";
    if(_wcsnicmp(argv[2],prefix,wcslen(prefix))!=0)return 2;
    wchar_t inf[MAX_PATH]={};
    const DWORD length=GetFullPathNameW(argv[3],MAX_PATH,inf,nullptr);
    if(!length || length>=MAX_PATH || GetFileAttributesW(inf)==INVALID_FILE_ATTRIBUTES)return fail("INF path");
    DeviceSet set;set.h=SetupDiCreateDeviceInfoList(nullptr,nullptr);
    if(set.h==INVALID_HANDLE_VALUE)return fail("CreateDeviceInfoList");
    SP_DEVINFO_DATA device={};device.cbSize=sizeof(device);
    if(!SetupDiOpenDeviceInfoW(set.h,argv[2],nullptr,0,&device))return fail("OpenDeviceInfo");
    SP_DEVINSTALL_PARAMS_W params={};params.cbSize=sizeof(params);
    if(!SetupDiGetDeviceInstallParamsW(set.h,&device,&params))return fail("GetDeviceInstallParams");
    // These are process-local device information-set parameters, not registry edits.
    params.Flags|=DI_ENUMSINGLEINF|DI_QUIETINSTALL|DI_DONOTCALLCONFIGMG;
    if(wcscpy_s(params.DriverPath,inf))return 2;
    if(!SetupDiSetDeviceInstallParamsW(set.h,&device,&params))return fail("SetDeviceInstallParams");
    if(!SetupDiBuildDriverInfoList(set.h,&device,SPDIT_COMPATDRIVER))return fail("BuildDriverInfoList");
    DWORD count=0;ULONGLONG version=0;
    for(DWORD index=0;;++index){
        SP_DRVINFO_DATA_W driver={};driver.cbSize=sizeof(driver);
        if(!SetupDiEnumDriverInfoW(set.h,&device,SPDIT_COMPATDRIVER,index,&driver)){
            if(GetLastError()!=ERROR_NO_MORE_ITEMS)return fail("EnumDriverInfo");
            break;
        }
        ++count;version=driver.DriverVersion;
    }
    ULONG status=0,problem=0;
    const CONFIGRET cr=CM_Get_DevNode_Status(&status,&problem,device.DevInst,0);
    printf("{\"read_only\":true,\"compatible_nodes\":%lu,\"driver_version\":\"%u.%u.%u.%u\",\"cm_result\":%lu,\"devnode_status\":%lu,\"problem\":%lu}\n",
        count,unsigned(version>>48),unsigned((version>>32)&0xffff),
        unsigned((version>>16)&0xffff),unsigned(version&0xffff),ULONG(cr),status,problem);
    SetupDiDestroyDriverInfoList(set.h,&device,SPDIT_COMPATDRIVER);
    return count==1 && cr==CR_SUCCESS?0:1;
}
