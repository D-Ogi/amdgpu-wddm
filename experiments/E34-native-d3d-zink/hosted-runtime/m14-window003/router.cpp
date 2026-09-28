#include "selection.h"
// Temporary system-UMD routing for one admitted process. The selected module
// stays loaded until process exit because its DDI callbacks outlive OpenAdapter.
static bool Selected() {
    wchar_t path[MAX_PATH]={},flag[4]={};
    DWORD count=GetModuleFileNameW(nullptr,path,MAX_PATH);
    if(!count || count>=MAX_PATH)return false;
    if(GetEnvironmentVariableW(L"BC250_M14_RUNTIME_PROBE",flag,4)!=1)return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExW(LR"(C:\BC250\m14\window003\enable)",GetFileExInfoStandard,&data) ||
       (data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return false;
    FILETIME now;GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER current{},enabled{};
    current.LowPart=now.dwLowDateTime;current.HighPart=now.dwHighDateTime;
    enabled.LowPart=data.ftLastWriteTime.dwLowDateTime;enabled.HighPart=data.ftLastWriteTime.dwHighDateTime;
    return m14_probe::select(path,flag,current.QuadPart,enabled.QuadPart);
}
static HRESULT Forward(const char *entry,void *args) {
    static const bool selected=Selected();
    static HMODULE module=LoadLibraryExW(selected?
        LR"(C:\BC250\m14\window003\bc250d3d11.dll)":
        LR"(C:\BC250\m14\window003\bc250d3d-cpu.dll)",nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)return E_FAIL;
    auto fn=reinterpret_cast<HRESULT(WINAPI*)(void*)>(GetProcAddress(module,entry));
    return fn?fn(args):E_NOINTERFACE;
}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10(void *args){return Forward("OpenAdapter10",args);}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10_2(void *args){return Forward("OpenAdapter10_2",args);}
