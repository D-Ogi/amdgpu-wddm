#include <windows.h>
static bool Selected(){
 wchar_t path[MAX_PATH]={};DWORD n=GetModuleFileNameW(nullptr,path,MAX_PATH);
 if(!n||n>=MAX_PATH||_wcsicmp(path,LR"(C:\BC250\m13\window-client001\gpu-window-control.exe)"))return false;
 if(!GetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE",nullptr,0))return false;
 WIN32_FILE_ATTRIBUTE_DATA data={};
 if(!GetFileAttributesExW(LR"(C:\BC250\m13\window-client001\enable)",GetFileExInfoStandard,&data))return false;
 FILETIME now;GetSystemTimeAsFileTime(&now);ULARGE_INTEGER a,b;
 a.LowPart=now.dwLowDateTime;a.HighPart=now.dwHighDateTime;
 b.LowPart=data.ftLastWriteTime.dwLowDateTime;b.HighPart=data.ftLastWriteTime.dwHighDateTime;
 return a.QuadPart>=b.QuadPart&&a.QuadPart-b.QuadPart<600000000ULL;
}
static HRESULT Forward(const char *entry,void *args){
 static const bool selected=Selected();
 HMODULE module=LoadLibraryW(selected?LR"(C:\BC250\m13\window-client001\bc250d3d_zink.dll)":LR"(C:\BC250\m13\window-client001\baseline-umd.dll)");
 if(!module)return HRESULT_FROM_WIN32(GetLastError());
 auto fn=reinterpret_cast<HRESULT(WINAPI*)(void*)>(GetProcAddress(module,entry));
 return fn?fn(args):E_NOINTERFACE;
}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10(void *args){return Forward("OpenAdapter10",args);}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10_2(void *args){return Forward("OpenAdapter10_2",args);}
