// SPDX-License-Identifier: MIT
#pragma once
#include "../d3d12/memory-policy.h"

namespace bc250::umd {
// BD-153: an FL12 pipeline does not establish the KMD's WDDM version. Query the OS on this adapter's LUID for
// the KMD's driver version before the adapter offers an interface newer than WDDM 2.0 (newest_ddi_level in
// ddi-negotiation.h). Source: SDK 26100 d3dkmthk.h, KMTQAITYPE_DRIVERVERSION / D3DKMT_DRIVERVERSION.
// The version is written only when the query succeeds; on a failure it stays KMT_DRIVERVERSION_WDDM_1_0.
inline HRESULT query_kmt_driver_version(const LUID &luid,const native12::MemoryPolicyKmt &kmt,
                                        D3DKMT_DRIVERVERSION &version,D3DKMT_HANDLE &unresolved) noexcept {
    version=KMT_DRIVERVERSION_WDDM_1_0; unresolved=0;
    if (!kmt.open || !kmt.query || !kmt.close) return E_INVALIDARG;
    D3DKMT_OPENADAPTERFROMLUID opened{}; opened.AdapterLuid=luid;
    const NTSTATUS status=kmt.open(&opened);
    if (status<0) return native12::memory_policy_detail::status_result(status);
    native12::memory_policy_detail::AdapterHandle adapter(opened.hAdapter,kmt.close,&unresolved);
    HRESULT result=S_OK;
    D3DKMT_DRIVERVERSION answer=KMT_DRIVERVERSION_WDDM_1_0;
    if (status!=0 || !opened.hAdapter) result=E_UNEXPECTED;
    else {
        D3DKMT_QUERYADAPTERINFO query{}; query.hAdapter=opened.hAdapter;
        query.Type=KMTQAITYPE_DRIVERVERSION;
        query.pPrivateDriverData=&answer; query.PrivateDriverDataSize=sizeof(answer);
        result=native12::memory_policy_detail::status_result(kmt.query(&query));
    }
    const HRESULT closed=adapter.close();
    if (closed!=S_OK) return closed;
    if (result==S_OK) version=answer;
    return result;
}
}
