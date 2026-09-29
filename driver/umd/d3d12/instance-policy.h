// SPDX-License-Identifier: MIT
#pragma once
#include "memory-policy.h"
#include <cwchar>

namespace native12 {
// What the driver decides for every instance it creates in the hosted ICD, resolved once per adapter before
// the capability query, so that the query and every device are given the same answer.
enum class SparsePolicySource : unsigned {
    Default,      // the system said that the adapter key has no such value: sparse binding is on
    RegistryOn,   // the value is 1
    RegistryOff,  // the value is 0
    Invalid,      // the value has another size or number: off
    Unreadable,   // the adapter key could not be asked, or the answer does not say why it failed: off
};
struct SparsePolicy {
    bool sparse{};
    SparsePolicySource source{SparsePolicySource::Unreadable};
    NTSTATUS status{};
};
inline constexpr wchar_t kSparsePolicyValue[]=L"AmdgpuWddmSparseBinding";

// Reads the DWORD kSparsePolicyValue of the adapter's software key on the already admitted LUID. No device,
// context, queue, allocation or submission is created. Only a failed open or close is an error of the call:
// a key that cannot be asked resolves to off, a value the system names as not found to on. The payload's
// own FAIL is an unspecified error, not a statement that the value is missing, and resolves to off. A
// failed close returns its error and transfers the unresolved handle to the caller, as
// query_memory_policy does.
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
        policy->source=SparsePolicySource::Unreadable;
    } else if(info.Status!=D3DDDI_QUERYREGISTRY_STATUS_SUCCESS || info.OutputValueSize!=sizeof(DWORD) ||
              info.OutputDword>1){
        policy->source=SparsePolicySource::Invalid;
    } else {
        policy->sparse=info.OutputDword==1;
        policy->source=policy->sparse?SparsePolicySource::RegistryOn:SparsePolicySource::RegistryOff;
    }
    return S_OK;
}
}
