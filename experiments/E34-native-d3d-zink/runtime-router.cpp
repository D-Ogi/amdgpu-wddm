#include <windows.h>
// Temporary E34 loader router. Keep the selected module loaded for callback lifetime.
static HRESULT Forward(const char *entry, void *args) {
 const bool probe=GetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE",nullptr,0)!=0;
 const wchar_t *path=probe ? L"C:\\BC250\\m13\\runtime-probe001\\bc250d3d_zink.dll" : L"C:\\BC250\\m11\\resource-close\\bc250d3d.m13-original.dll";
 HMODULE module=LoadLibraryW(path);
 if(!module)return HRESULT_FROM_WIN32(GetLastError());
 auto fn=(HRESULT(WINAPI*)(void*))GetProcAddress(module,entry);
 if(!fn)return E_NOINTERFACE;
 return fn(args);
}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10(void *args){return Forward("OpenAdapter10",args);}
extern "C" __declspec(dllexport) HRESULT WINAPI OpenAdapter10_2(void *args){return Forward("OpenAdapter10_2",args);}
