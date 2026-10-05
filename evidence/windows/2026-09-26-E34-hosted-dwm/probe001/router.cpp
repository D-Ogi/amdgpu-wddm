#include <windows.h>
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
 if(!_wcsicmp(base,L"dwm.exe") && GetFileAttributesExW(L"C:\\BC250\\m13\\dwm-hosted001\\enable",GetFileExInfoStandard,&attr)) {
  t.LowPart=attr.ftLastWriteTime.dwLowDateTime;t.HighPart=attr.ftLastWriteTime.dwHighDateTime;
  hosted=n.QuadPart>=t.QuadPart && n.QuadPart-t.QuadPart<300000000ULL;
 }
 if(hosted) {
  HANDLE claim=CreateFileW(L"C:\\BC250\\m13\\dwm-hosted001\\claim",GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(claim==INVALID_HANDLE_VALUE)hosted=false;
  else {DWORD pid=GetCurrentProcessId(),written;WriteFile(claim,&pid,sizeof(pid),&written,nullptr);CloseHandle(claim);}
 }
 if(hosted) {
  wchar_t log[MAX_PATH];swprintf_s(log,L"C:\\BC250\\m13\\dwm-hosted001\\dwm-%lu.log",GetCurrentProcessId());
  FILE *f=nullptr;_wfreopen_s(&f,log,L"a",stderr);setvbuf(stderr,nullptr,_IONBF,0);
  char luid[32]={};FILE *cfg=nullptr;
  if(fopen_s(&cfg,"C:\\BC250\\m13\\dwm-hosted001\\luid.txt","rb") || !cfg){loadError=ERROR_INVALID_DATA;return TRUE;}
  size_t len=fread(luid,1,16,cfg);fclose(cfg);
  bool valid=len==16;for(size_t i=0;i<len;i++)valid=valid&&((luid[i]>='0'&&luid[i]<='9')||(luid[i]>='a'&&luid[i]<='f')||(luid[i]>='A'&&luid[i]<='F'));
  if(!valid){loadError=ERROR_INVALID_DATA;return TRUE;}
  SetEnvironmentVariableA("BC250_D3D_ZINK_LUID",luid);
  SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE","1");
  SetEnvironmentVariableA("BC250_HOSTED_RENDER","1");
  SetEnvironmentVariableA("BC250_HOST_TEST_LOSS",nullptr);
  SetEnvironmentVariableA("BC250_HOSTED_ICD","C:\\BC250\\m13\\dwm-hosted001\\vulkan_radeon.dll");
  SetEnvironmentVariableA("MESA_SHADER_CACHE_DIR","C:\\BC250\\m13\\dwm-hosted001\\cache");
  SetEnvironmentVariableW(L"MESA_LOG_FILE",log);
  fprintf(stderr,"DWM route pid=%lu luid=%s hosted=1\n",GetCurrentProcessId(),luid);
 }
 module=LoadLibraryW(hosted?L"C:\\BC250\\m13\\dwm-hosted001\\bc250d3d_zink.dll":L"C:\\BC250\\m13\\dwm-hosted001\\baseline-umd.dll");
 if(!module)loadError=GetLastError();
 return TRUE;
}
static HRESULT Forward(const char *entry,void *args){
 InitOnceExecuteOnce(&once,Initialize,nullptr,nullptr);
 if(!module)return HRESULT_FROM_WIN32(loadError?loadError:ERROR_MOD_NOT_FOUND);
 auto fn=(HRESULT(WINAPI*)(void*))GetProcAddress(module,entry);
 if(!fn)return E_NOINTERFACE;
 HRESULT hr=fn(args);
 if(GetEnvironmentVariableA("BC250_HOSTED_RENDER",nullptr,0))fprintf(stderr,"DWM route %s hr=%08lx pid=%lu\n",entry,hr,GetCurrentProcessId());
 return hr;
}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10(void *args){return Forward("OpenAdapter10",args);}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10_2(void *args){return Forward("OpenAdapter10_2",args);}
