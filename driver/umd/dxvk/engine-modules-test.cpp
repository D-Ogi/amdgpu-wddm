// SPDX-License-Identifier: MIT
#include "engine-abi.h"
#include "engine-modules.h"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace bc250::umd;
int wmain(int argc,wchar_t **argv) {
    if (argc!=2) return 2;
    for (const wchar_t *p:std::initializer_list<const wchar_t *>{nullptr,L"",L"x",L"C:",L"C:engine.dll",L"engine.dll",L"\\engine.dll",L"\\\\host\\share\\engine.dll"})
        if (EngineModules::absolute_path(p)) std::abort();
    if (!EngineModules::absolute_path(argv[1])) std::abort();
    EngineModules modules;
    if (modules.open(L"engine.dll",argv[1])!=E_INVALIDARG || modules.loaded()) std::abort();
    if (modules.open(argv[1],argv[1])!=S_OK || !modules.loaded() || !modules.functions().CreateDevice) std::abort();
    if (modules.open(argv[1],argv[1])!=E_UNEXPECTED || !modules.loaded()) std::abort();
    modules.close(); modules.close();
    if (modules.loaded() || modules.functions().CreateDevice || modules.get_instance_proc_addr()) std::abort();
    const std::wstring missing=std::wstring(argv[1])+L".absent";
    if (SUCCEEDED(modules.open(argv[1],missing.c_str())) || modules.loaded() || modules.functions().CreateDevice) std::abort();
    if (modules.open(argv[1],argv[1])!=S_OK) std::abort();
    modules.close();
    HMODULE fixture=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!fixture) std::abort();
    auto mode=reinterpret_cast<void (APIENTRY *)(UINT)>(GetProcAddress(fixture,"SetFixtureMode"));
    if (!mode) std::abort();
    for (UINT i=1;i<=3;++i) {
        mode(i);
        const HRESULT expected=i==3 ? E_FAIL : E_NOINTERFACE;
        if (modules.open(argv[1],argv[1])!=expected || modules.loaded() || modules.functions().CreateDevice) std::abort();
    }
    if (!compatible_engine_abi(0x10001,0x10000) || compatible_engine_abi(0x10000,0x10001) || compatible_engine_abi(0x20000,0x10000)) std::abort();
    mode(4);
    if (modules.open(argv[1],argv[1])!=S_OK) std::abort();
    modules.close();
    if (!FreeLibrary(fixture)) std::abort();
    std::cout << "PASS DLL loader positive fixture, rollback, retry and path controls (no GPU)\n";
}
