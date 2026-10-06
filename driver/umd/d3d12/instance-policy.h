// SPDX-License-Identifier: MIT
#pragma once
#include "memory-policy.h"
#include <cwchar>

namespace native12 {
// What the driver decides for every instance it creates in the hosted ICD, resolved once per adapter before
// the capability query, so that the query and every device are given the same answer.
// Only an explicit 0 turns sparse binding off. Everything else, a missing value included, leaves the driver's
// validated default in place, because nothing that fails to read a registry value is evidence about the
// hardware: resolving a failed read to off used to drop feature level 12_1 and tiled resources tier 3 in
// silence.
enum class SparsePolicySource : unsigned {
    Default,      // the system said that the adapter key has no such value: sparse binding is on
    RegistryOn,   // the value is 1
    RegistryOff,  // the value is 0: the only answer that turns it off
    Invalid,      // the value has another size or number: on, and logged
    Unreadable,   // the adapter key could not be asked, or the answer does not say why it failed: on, logged
};
struct SparsePolicy {
    bool sparse{};
    SparsePolicySource source{SparsePolicySource::Unreadable};
    NTSTATUS status{};
};
inline constexpr wchar_t kSparsePolicyValue[]=L"AmdgpuWddmSparseBinding";

// Reads the DWORD kSparsePolicyValue of the adapter's software key on the already admitted LUID. No device,
// context, queue, allocation or submission is created. Only a failed open or close is an error of the call:
// a value the system names as not found, a key that cannot be asked and a value outside {0,1} all resolve to
// on, the driver's default, and the source says which so that the caller can log it. The payload's own FAIL
// is an unspecified error, not a statement that the value is 0. A failed close returns its error and
// transfers the unresolved handle to the caller, as query_memory_policy does; sparse stays off on that path,
// because the caller abandons the adapter.
// Sources: WDK 26100 d3dukmdt.h D3DDDI_QUERYREGISTRY_INFO, d3dkmthk.h KMTQAITYPE_QUERYREGISTRY.
inline HRESULT query_sparse_policy(const LUID& luid,const MemoryPolicyKmt& kmt,SparsePolicy* policy,
                                   D3DKMT_HANDLE* unresolved_adapter) noexcept {
    if(policy)*policy={};
    if(unresolved_adapter)*unresolved_adapter=0;
    if(!policy || !unresolved_adapter || !kmt.open || !kmt.query || !kmt.close)return E_INVALIDARG;
    D3DKMT_OPENADAPTERFROMLUID opened{};opened.AdapterLuid=luid;
    NTSTATUS status=kmt.open(&opened);
    if(status<0)return memory_policy_detail::status_result(status);
    memory_policy_detail::AdapterHandle adapter(opened.hAdapter,kmt.close,unresolved_adapter);
    if(status!=0 || !opened.hAdapter){
        const HRESULT closed=adapter.close();
        return closed!=S_OK?closed:E_UNEXPECTED;
    }
    D3DDDI_QUERYREGISTRY_INFO info{};
    info.QueryType=D3DDDI_QUERYREGISTRY_ADAPTERKEY;
    static_assert(sizeof(kSparsePolicyValue)<=sizeof(info.ValueName));
    std::wmemcpy(info.ValueName,kSparsePolicyValue,sizeof(kSparsePolicyValue)/sizeof(wchar_t));
    info.ValueType=REG_DWORD;
    D3DKMT_QUERYADAPTERINFO query{};query.hAdapter=opened.hAdapter;
    query.Type=KMTQAITYPE_QUERYREGISTRY;
    query.pPrivateDriverData=&info;query.PrivateDriverDataSize=sizeof(info);
    status=kmt.query(&query);
    const HRESULT closed=adapter.close();
    if(closed!=S_OK)return closed;
    policy->status=status;
    constexpr NTSTATUS not_found=static_cast<NTSTATUS>(0xC0000034u); // STATUS_OBJECT_NAME_NOT_FOUND
    if(status==not_found){
        policy->sparse=true;policy->source=SparsePolicySource::Default;
    } else if(status!=0 || info.Status==D3DDDI_QUERYREGISTRY_STATUS_FAIL){
        policy->sparse=true;policy->source=SparsePolicySource::Unreadable;
    } else if(info.Status!=D3DDDI_QUERYREGISTRY_STATUS_SUCCESS || info.OutputValueSize!=sizeof(DWORD) ||
              info.OutputDword>1){
        policy->sparse=true;policy->source=SparsePolicySource::Invalid;
    } else {
        policy->sparse=info.OutputDword==1;
        policy->source=policy->sparse?SparsePolicySource::RegistryOn:SparsePolicySource::RegistryOff;
    }
    return S_OK;
}
}
