// SPDX-License-Identifier: MIT
#pragma once
#include "../d3d12/memory-policy.h"

namespace bc250::umd {
// BD-153: an FL12 pipeline does not establish the KMD's WDDM version. Query the
// OS on this adapter's LUID before offering the optional WDDM 2.2 D3D11 table.
// Source: SDK 26100 d3dkmthk.h, KMTQAITYPE_DRIVERVERSION / D3DKMT_DRIVERVERSION.
// The base WDDM 2.0 table remains the minimum for the existing FL12 route.
inline HRESULT query_wddm22_support(const LUID &luid,const native12::MemoryPolicyKmt &kmt,
                                   bool &supported,D3DKMT_HANDLE &unresolved) noexcept {
    supported=false; unresolved=0;
    if (!kmt.open || !kmt.query || !kmt.close) return E_INVALIDARG;
    D3DKMT_OPENADAPTERFROMLUID opened{}; opened.AdapterLuid=luid;
    const NTSTATUS status=kmt.open(&opened);
    if (status<0) return native12::memory_policy_detail::status_result(status);
    native12::memory_policy_detail::AdapterHandle adapter(opened.hAdapter,kmt.close,&unresolved);
    HRESULT result=S_OK;
    D3DKMT_DRIVERVERSION version=KMT_DRIVERVERSION_WDDM_1_0;
    if (status!=0 || !opened.hAdapter) result=E_UNEXPECTED;
    else {
        D3DKMT_QUERYADAPTERINFO query{}; query.hAdapter=opened.hAdapter;
        query.Type=KMTQAITYPE_DRIVERVERSION;
        query.pPrivateDriverData=&version; query.PrivateDriverDataSize=sizeof(version);
        result=native12::memory_policy_detail::status_result(kmt.query(&query));
    }
    const HRESULT closed=adapter.close();
    if (closed!=S_OK) return closed;
    if (result==S_OK) supported=version>=KMT_DRIVERVERSION_WDDM_2_2;
    return result;
}
}
