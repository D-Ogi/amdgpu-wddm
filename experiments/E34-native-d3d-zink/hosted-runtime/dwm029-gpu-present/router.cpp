#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#pragma warning(pop)
#include <cstdio>
#include <cwchar>
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static HMODULE module;
static DWORD loadError;
static BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
 wchar_t exe[MAX_PATH]={}; GetModuleFileNameW(nullptr,exe,MAX_PATH);
 const wchar_t *base=wcsrchr(exe,L'\\');base=base?base+1:exe;
 bool hosted=false;
 WIN32_FILE_ATTRIBUTE_DATA attr={};FILETIME now;
 GetSystemTimeAsFileTime(&now);
 ULARGE_INTEGER n,t;n.LowPart=now.dwLowDateTime;n.HighPart=now.dwHighDateTime;
 if(!_wcsicmp(base,L"dwm.exe") && GetFileAttributesExW(L"C:\\BC250\\m13\\dwm-hosted029\\enable",GetFileExInfoStandard,&attr)) {
  t.LowPart=attr.ftLastWriteTime.dwLowDateTime;t.HighPart=attr.ftLastWriteTime.dwHighDateTime;
  hosted=n.QuadPart>=t.QuadPart && n.QuadPart-t.QuadPart<600000000ULL;
 }
 if(hosted) {
  HANDLE claim=CreateFileW(L"C:\\BC250\\m13\\dwm-hosted029\\claim",GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(claim==INVALID_HANDLE_VALUE) {
   claim=CreateFileW(L"C:\\BC250\\m13\\dwm-hosted029\\claim",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
   DWORD owner=0,read=0;
   hosted=claim!=INVALID_HANDLE_VALUE && ReadFile(claim,&owner,sizeof(owner),&read,nullptr) && read==sizeof(owner) && owner==GetCurrentProcessId();
   if(claim!=INVALID_HANDLE_VALUE)CloseHandle(claim);
  }
  else {DWORD pid=GetCurrentProcessId(),written;WriteFile(claim,&pid,sizeof(pid),&written,nullptr);CloseHandle(claim);}
 }
 if(hosted) {
  wchar_t log[MAX_PATH];swprintf_s(log,L"C:\\BC250\\m13\\dwm-hosted029\\dwm-%lu.log",GetCurrentProcessId());
  FILE *f=nullptr;_wfreopen_s(&f,log,L"a",stderr);setvbuf(stderr,nullptr,_IONBF,0);
  char luid[32]={};FILE *cfg=nullptr;
  if(fopen_s(&cfg,"C:\\BC250\\m13\\dwm-hosted029\\luid.txt","rb") || !cfg){loadError=ERROR_INVALID_DATA;return TRUE;}
  size_t len=fread(luid,1,16,cfg);fclose(cfg);
  bool valid=len==16;for(size_t i=0;i<len;i++)valid=valid&&((luid[i]>='0'&&luid[i]<='9')||(luid[i]>='a'&&luid[i]<='f')||(luid[i]>='A'&&luid[i]<='F'));
  if(!valid){loadError=ERROR_INVALID_DATA;return TRUE;}
  SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",luid);
  SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1");
  SetEnvironmentVariableA("BC250_HOSTED_RENDER","1");
  SetEnvironmentVariableA("BC250_HOST_TRACE_DDI",nullptr);
  SetEnvironmentVariableA("BC250_HOST_AUDIT","1");
  SetEnvironmentVariableA("BC250_HOST_DIAG_DRAW_IDLE",nullptr);
  SetEnvironmentVariableA("BC250_HOST_TEST_LOSS",nullptr);
  SetEnvironmentVariableA("BC250_HOSTED_ICD","C:\\BC250\\m13\\dwm-hosted029\\vulkan_radeon.dll");
  SetEnvironmentVariableA("MESA_SHADER_CACHE_DIR","C:\\BC250\\m13\\dwm-hosted029\\cache");
  SetEnvironmentVariableW(L"MESA_LOG_FILE",log);
  fprintf(stderr,"DWM route pid=%lu luid=%s hosted=1\n",GetCurrentProcessId(),luid);
 }
 wchar_t routePath[MAX_PATH];swprintf_s(routePath,L"C:\\BC250\\m13\\dwm-hosted029\\route-%lu.log",GetCurrentProcessId());
 FILE *route=nullptr;_wfopen_s(&route,routePath,L"a");
 if(route){fprintf(route,"pid=%lu hosted=%u\n",GetCurrentProcessId(),hosted?1u:0u);fclose(route);}
 module=LoadLibraryW(hosted?L"C:\\BC250\\m13\\dwm-hosted029\\bc250d3d_zink.dll":L"C:\\BC250\\m13\\dwm-hosted029\\baseline-umd.dll");
 if(!module)loadError=GetLastError();
 return TRUE;
}
static D3D10_2DDI_ADAPTERFUNCS original;
static HRESULT APIENTRY Versions(D3D10DDI_HADAPTER a,UINT32 *count,UINT64 *versions){
 UINT32 requested=*count;HRESULT hr=original.pfnGetSupportedVersions(a,count,versions);
 fprintf(stderr,"DWM Versions requested=%u supplied=%d count=%u hr=%08lx\n",requested,versions!=nullptr,*count,hr);
 if(SUCCEEDED(hr)&&versions)for(UINT32 i=0;i<*count;i++)fprintf(stderr," version=%016llx\n",versions[i]);
 return hr;
}
static HRESULT APIENTRY Caps(D3D10DDI_HADAPTER a,const D3D10_2DDIARG_GETCAPS *c){
 HRESULT hr=original.pfnGetCaps(a,c);fprintf(stderr,"DWM Caps type=%u size=%u hr=%08lx\n",c->Type,c->DataSize,hr);return hr;
}
static SIZE_T APIENTRY Size(D3D10DDI_HADAPTER a,const D3D10DDIARG_CALCPRIVATEDEVICESIZE *c){
 SIZE_T n=original.pfnCalcPrivateDeviceSize(a,c);fprintf(stderr,"DWM DeviceSize interface=%08x version=%08x bytes=%zu\n",c->Interface,c->Version,n);return n;
}
static HRESULT APIENTRY Create(D3D10DDI_HADAPTER a,D3D10DDIARG_CREATEDEVICE *c){
 fprintf(stderr,"DWM CreateDevice interface=%08x version=%08x\n",c->Interface,c->Version);
 HRESULT hr=original.pfnCreateDevice(a,c);fprintf(stderr,"DWM CreateDevice hr=%08lx\n",hr);return hr;
}
static HRESULT Forward(const char *entry,void *args){
 InitOnceExecuteOnce(&once,Initialize,nullptr,nullptr);
 if(!module)return HRESULT_FROM_WIN32(loadError?loadError:ERROR_MOD_NOT_FOUND);
 auto fn=(HRESULT(WINAPI*)(void*))GetProcAddress(module,entry);
 if(!fn)return E_NOINTERFACE;
 HRESULT hr=fn(args);
 if(SUCCEEDED(hr)&&!strcmp(entry,"OpenAdapter10_2")&&GetEnvironmentVariableA("BC250_HOSTED_RENDER",nullptr,0)) {
  auto *data=static_cast<D3D10DDIARG_OPENADAPTER*>(args);auto *table=data->pAdapterFuncs_2;
  original=*table;table->pfnGetSupportedVersions=Versions;table->pfnGetCaps=Caps;
  table->pfnCalcPrivateDeviceSize=Size;table->pfnCreateDevice=Create;
 }

 if(GetEnvironmentVariableA("BC250_HOSTED_RENDER",nullptr,0))fprintf(stderr,"DWM route %s hr=%08lx pid=%lu\n",entry,hr,GetCurrentProcessId());
 return hr;
}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10(void *args){return Forward("OpenAdapter10",args);}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10_2(void *args){return Forward("OpenAdapter10_2",args);}
