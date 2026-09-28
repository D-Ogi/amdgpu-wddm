// Exact-INF probe and explicit deferred installation. --inspect never installs.
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cfg.h>
#include <cstdio>
#include <cwchar>
#include <string>
struct DeviceSet {
    HDEVINFO h=INVALID_HANDLE_VALUE;
    ~DeviceSet(){if(h!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(h);}
};
static int fail(const char* operation){
    fprintf(stderr,"%s failed: %lu\n",operation,GetLastError());return 1;
}
// Encode UTF-16 code units as JSON escapes, including surrogate pairs.
static std::string jsonPath(const wchar_t* path){
    std::string result="\"";
    for(;*path;++path){
        char encoded[7]={};
        sprintf_s(encoded,"\\u%04x",unsigned(*path));result+=encoded;
    }
    return result+"\"";
}
static bool parseVersion(const wchar_t* text,ULONGLONG& value){
    unsigned a=0,b=0,c=0,d=0;wchar_t tail=0;
    if(swscanf_s(text,L"%u.%u.%u.%u%c",&a,&b,&c,&d,&tail,1u)!=4 ||
       a>65535 || b>65535 || c>65535 || d>65535)return false;
    value=(ULONGLONG(a)<<48)|(ULONGLONG(b)<<32)|(ULONGLONG(c)<<16)|d;return true;
}
static bool disabled(CONFIGRET cr,ULONG status,ULONG problem){
    return cr==CR_SUCCESS && (status&DN_HAS_PROBLEM) && !(status&DN_STARTED) && problem==CM_PROB_DISABLED;
}
int wmain(int argc,wchar_t** argv){
    if(argc==2 && wcscmp(argv[1],L"--help")==0){
        puts("select-driver --inspect INSTANCE ABSOLUTE_INF; --inspect-store INSTANCE PUBLISHED_INF; or --install-deferred INSTANCE ABSOLUTE_INF EXPECTED_VERSION");return 0;
    }
    const bool install=argc==5 && wcscmp(argv[1],L"--install-deferred")==0;
    const bool store=argc==4 && wcscmp(argv[1],L"--inspect-store")==0;
    if(!install && !store && (argc!=4 || wcscmp(argv[1],L"--inspect")!=0))return 2;
    ULONGLONG expectedVersion=0;
    if(install && !parseVersion(argv[4],expectedVersion))return 2;
    const wchar_t prefix[]=L"PCI\\VEN_1002&DEV_13FE";
    if(_wcsnicmp(argv[2],prefix,wcslen(prefix))!=0 ||
       (argv[2][wcslen(prefix)]!=L'&' && argv[2][wcslen(prefix)]!=L'\\'))return 2;
    wchar_t inf[MAX_PATH]={};
    const DWORD length=GetFullPathNameW(argv[3],MAX_PATH,inf,nullptr);
    if(!length || length>=MAX_PATH || GetFileAttributesW(inf)==INVALID_FILE_ATTRIBUTES)return fail("INF path");
    if(store){
        wchar_t resolved[MAX_PATH]={};
        // Only published/system INF or Driver Store paths are supported inputs.
        // This is a lookup, not content matching or package staging.
        if(!SetupGetInfDriverStoreLocationW(inf,nullptr,nullptr,resolved,MAX_PATH,nullptr))
            return fail("GetInfDriverStoreLocation");
        printf("{\"resolved_store_inf\":%s}\n",jsonPath(resolved).c_str());
        if(wcscpy_s(inf,resolved))return 2;
    }
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
    DWORD count=0;ULONGLONG version=0;SP_DRVINFO_DATA_W selected={};
    for(DWORD index=0;;++index){
        SP_DRVINFO_DATA_W driver={};driver.cbSize=sizeof(driver);
        if(!SetupDiEnumDriverInfoW(set.h,&device,SPDIT_COMPATDRIVER,index,&driver)){
            if(GetLastError()!=ERROR_NO_MORE_ITEMS)return fail("EnumDriverInfo");
            break;
        }
        ++count;version=driver.DriverVersion;selected=driver;
        SP_DRVINFO_DETAIL_DATA_W detail={};detail.cbSize=sizeof(detail);
        // Only static fields are needed. The documented insufficient-buffer
        // result still populates them; no truncated hardware ID list is read.
        if(!SetupDiGetDriverInfoDetailW(set.h,&device,&driver,&detail,sizeof(detail),nullptr) &&
           GetLastError()!=ERROR_INSUFFICIENT_BUFFER)return fail("GetDriverInfoDetail");
        printf("{\"node_index\":%lu,\"node_inf\":%s,\"section\":%s}\n",
               index,jsonPath(detail.InfFileName).c_str(),jsonPath(detail.SectionName).c_str());
    }
    ULONG status=0,problem=0;
    const CONFIGRET cr=CM_Get_DevNode_Status(&status,&problem,device.DevInst,0);
    printf("{\"read_only\":%s,\"compatible_nodes\":%lu,\"driver_version\":\"%u.%u.%u.%u\",\"cm_result\":%lu,\"devnode_status\":%lu,\"problem\":%lu}\n",
        install?"false":"true",count,unsigned(version>>48),unsigned((version>>32)&0xffff),
        unsigned((version>>16)&0xffff),unsigned(version&0xffff),ULONG(cr),status,problem);
    fflush(stdout);
    if(install){
        if(count!=1 || version!=expectedVersion || !disabled(cr,status,problem)){
            fputs("Deferred install requires exact version, one node and disabled device\n",stderr);return 3;
        }
        if(!SetupDiSetSelectedDriverW(set.h,&device,&selected))return fail("SetSelectedDriver");
        if(!SetupDiGetDeviceInstallParamsW(set.h,&device,&params))return fail("GetInstallParams before DIF");
        params.Flags|=DI_DONOTCALLCONFIGMG|DI_QUIETINSTALL;
        if(!SetupDiSetDeviceInstallParamsW(set.h,&device,&params))return fail("SetInstallParams before DIF");
        // An application requests class installation, never calls the default handler directly.
        // SDK26100 contracts: SetupDiCallClassInstaller / DIF_INSTALLDEVICE.
        if(!SetupDiCallClassInstaller(DIF_INSTALLDEVICE,set.h,&device))return fail("DIF_INSTALLDEVICE");
        if(!SetupDiGetDeviceInstallParamsW(set.h,&device,&params))return fail("GetInstallParams after DIF");
        const CONFIGRET after=CM_Get_DevNode_Status(&status,&problem,device.DevInst,0);
        const bool accepted=disabled(after,status,problem) &&
            (params.Flags&DI_DONOTCALLCONFIGMG)!=0 &&
            (params.Flags&(DI_NEEDREBOOT|DI_NEEDRESTART))==0;
        printf("{\"installed_deferred\":%s,\"install_flags\":%lu,\"cm_result\":%lu,\"devnode_status\":%lu,\"problem\":%lu}\n",
               accepted?"true":"false",params.Flags,ULONG(after),status,problem);
        // This process-local information set is destroyed below. No CM state-change
        // operation is issued while suppression is set. A later stage verifies the
        // installed SYS/INF, restores configuration and separately enables the device.
        if(!accepted)return 4;
    }
    SetupDiDestroyDriverInfoList(set.h,&device,SPDIT_COMPATDRIVER);
    return count==1 && cr==CR_SUCCESS?0:1;
}
