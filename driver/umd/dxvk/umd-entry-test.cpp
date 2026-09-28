// SPDX-License-Identifier: MIT
#include "adapter-config.h"
#include "../../contract/bc250_adapter_identity.h"
#include <string>
#include <cstdio>
#include <cstdlib>
using namespace bc250::umd;
#define CHECK(x) do{if(!(x)){std::printf("FAIL line %d\n",__LINE__);std::abort();}}while(0)
HRESULT APIENTRY query(HANDLE,const D3DDDICB_QUERYADAPTERINFO *args) {
    CHECK(args && args->PrivateDriverDataSize==BC250_ADAPTER_CAPS_BYTES);
    bc250_adapter_identity identity{BC250_ADAPTER_IDENTITY_MAGIC,1,24,123,0,0};
    std::memcpy(static_cast<unsigned char *>(args->pPrivateDriverData)+BC250_ADAPTER_IDENTITY_OFFSET,&identity,sizeof(identity));
    return S_OK;
}
void write_record(const std::wstring &path,const AdapterConfigRecord &record,DWORD length=sizeof(AdapterConfigRecord)) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(file!=INVALID_HANDLE_VALUE);DWORD count=0;
    CHECK(WriteFile(file,&record,length,&count,nullptr) && count==length);CloseHandle(file);
}
int wmain(int argc,wchar_t **argv) {
    CHECK(argc==2);
    const std::wstring dll=argv[1];const auto slash=dll.find_last_of(L"\\/");CHECK(slash!=std::wstring::npos);
    const auto config=dll.substr(0,slash+1)+L"amdgpu_wddm_d3d11.config";
    DeleteFileW(config.c_str());
    HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(module);
    auto open=reinterpret_cast<HRESULT(APIENTRY *)(D3D10DDIARG_OPENADAPTER *)>(GetProcAddress(module,"OpenAdapter10_2"));
    CHECK(open && open(nullptr)==E_INVALIDARG);
    D3D10_2DDI_ADAPTERFUNCS table{};std::memset(&table,0xA5,sizeof(table));const auto before=table;
    D3DDDI_ADAPTERCALLBACKS callbacks{};callbacks.pfnQueryAdapterInfoCb=query;
    D3D10DDIARG_OPENADAPTER args{};args.pAdapterCallbacks=&callbacks;args.pAdapterFuncs_2=&table;
    int identity=0;args.hRTAdapter.handle=&identity;args.hAdapter.pDrvPrivate=&identity;
    CHECK(FAILED(open(&args)) && args.hAdapter.pDrvPrivate==&identity && !std::memcmp(&before,&table,sizeof(table)));
    AdapterConfigRecord record{};record.magic=adapter_config_magic;record.version=1;record.size=sizeof(record);
    record.maximum=D3D_FEATURE_LEVEL_11_0;record.compute=1;
    std::memset(record.engine_sha256,1,32);std::memset(record.icd_sha256,2,32);
    write_record(config,record,sizeof(record)-1);CHECK(open(&args)==E_INVALIDARG);
    record.version=2;write_record(config,record);CHECK(open(&args)==E_INVALIDARG);record.version=1;
    record.reserved=1;write_record(config,record);CHECK(open(&args)==E_INVALIDARG);record.reserved=0;
    record.doubles=2;write_record(config,record);CHECK(open(&args)==E_INVALIDARG);record.doubles=0;
    record.compute=0;write_record(config,record);CHECK(open(&args)==E_INVALIDARG);record.compute=1;
    CHECK(args.hAdapter.pDrvPrivate==&identity && !std::memcmp(&before,&table,sizeof(table)));
    write_record(config,record);CHECK(open(&args)==S_OK && args.hAdapter.pDrvPrivate!=&identity);
    UINT32 entries=0;CHECK(table.pfnGetSupportedVersions(args.hAdapter,&entries,nullptr)==S_OK && entries==1);
    D3D11DDI_3DPIPELINESUPPORT_CAPS pipelines{};D3D10_2DDIARG_GETCAPS capArgs{};
    capArgs.Type=D3D11DDICAPS_3DPIPELINESUPPORT;capArgs.pData=&pipelines;capArgs.DataSize=sizeof(pipelines);
    CHECK(table.pfnGetCaps(args.hAdapter,&capArgs)==S_OK && pipelines.Caps==7);
    CHECK(table.pfnCloseAdapter(args.hAdapter)==S_OK);
    CHECK(FreeLibrary(module));CHECK(DeleteFileW(config.c_str()));
    std::puts("PASS exported OpenAdapter10_2: missing/malformed configuration, negotiation and close (no GPU)");
}
