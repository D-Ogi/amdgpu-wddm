// SPDX-License-Identifier: MIT
#include "engine-modules.h"
#include <cwchar>
namespace bc250::umd {
bool EngineModules::absolute_path(const wchar_t *p) {
    // Deployment is local: reject relative, drive-relative, UNC and device paths.
    if (!p || std::wcslen(p)<4) return false;
    const bool drive=(p[0]>=L'A' && p[0]<=L'Z') || (p[0]>=L'a' && p[0]<=L'z');
    return drive && p[1]==L':' && (p[2]==L'\\' || p[2]==L'/');
}
EngineModules::~EngineModules() { close(); }
void EngineModules::close() {
    functions_={}; get_=nullptr;
    if (icd_) { FreeLibrary(icd_); icd_=nullptr; }
    if (engine_) { FreeLibrary(engine_); engine_=nullptr; }
}
HRESULT EngineModules::open(const wchar_t *enginePath,const wchar_t *icdPath) {
    if (engine_ || icd_) return E_UNEXPECTED;
    if (!absolute_path(enginePath) || !absolute_path(icdPath)) return E_INVALIDARG;
    // Never search the application's current directory or PATH for dependencies.
    constexpr DWORD search=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    engine_=LoadLibraryExW(enginePath,nullptr,search);
    if (!engine_) return HRESULT_FROM_WIN32(GetLastError());
    auto getter=reinterpret_cast<PFN_BC250_DXVK_ENGINE_GET_FUNCS>(GetProcAddress(engine_,BC250_DXVK_ENGINE_GET_FUNCS_NAME));
    if (!getter) { close(); return E_NOINTERFACE; }
    functions_.Size=sizeof(functions_);
    HRESULT hr=getter(BC250_DXVK_ENGINE_ABI_VERSION,&functions_);
    if (SUCCEEDED(hr) && (functions_.Size<sizeof(functions_) || functions_.AbiVersion!=BC250_DXVK_ENGINE_ABI_VERSION ||
        !functions_.QueryDeviceRequirements || !functions_.FreeDeviceRequirements || !functions_.GetAdapterInfo || !functions_.CreateDevice)) hr=E_NOINTERFACE;
    if (FAILED(hr)) { close(); return hr; }
    icd_=LoadLibraryExW(icdPath,nullptr,search);
    if (!icd_) { hr=HRESULT_FROM_WIN32(GetLastError()); close(); return hr; }
    get_=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd_,"vk_icdGetInstanceProcAddr"));
    if (!get_) get_=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd_,"vkGetInstanceProcAddr"));
    if (!get_) { close(); return E_NOINTERFACE; }
    return S_OK;
}
}
