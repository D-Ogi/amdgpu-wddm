// SPDX-License-Identifier: MIT
#include "adapter-config.h"
#include <string>
#include <vector>
namespace {
void module_anchor() {}
HRESULT configuration_directory(std::wstring &directory) {
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&module_anchor),&module))return HRESULT_FROM_WIN32(GetLastError());
    std::vector<wchar_t> path(32768);
    DWORD count=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    if(!count)return HRESULT_FROM_WIN32(GetLastError());
    if(count>=path.size())return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    directory.assign(path.data(),count);
    const auto slash=directory.find_last_of(L"\\/");
    if(slash==std::wstring::npos)return E_FAIL;
    directory.resize(slash+1);return S_OK;
}
HRESULT read_record(const std::wstring &path,bc250::umd::AdapterConfigRecord &record) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
    struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{file};
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file,&size))return HRESULT_FROM_WIN32(GetLastError());
    if(size.QuadPart!=sizeof(record))return E_INVALIDARG;
    DWORD count=0;
    if(!ReadFile(file,&record,sizeof(record),&count,nullptr))return HRESULT_FROM_WIN32(GetLastError());
    return count==sizeof(record) ? S_OK : E_FAIL;
}
}
extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *args) {
    if(!args)return E_INVALIDARG;
    try {
        std::wstring directory;HRESULT hr=configuration_directory(directory);if(FAILED(hr))return hr;
        bc250::umd::AdapterConfigRecord record{};
        hr=read_record(directory+L"bc250d3d11.config",record);if(FAILED(hr))return hr;
        bc250::umd::AdapterConfiguration config{};
        hr=bc250::umd::decode_adapter_config(record,config);if(FAILED(hr))return hr;
        const std::wstring engine=directory+L"bc250dxvk.dll",icd=directory+L"bc250radv.dll";
        config.engine_path=engine.c_str();config.icd_path=icd.c_str();
        return bc250::umd::open_render_adapter(*args,config);
    } catch(const std::bad_alloc &) {return E_OUTOFMEMORY;}
      catch(...) {return E_FAIL;}
}
