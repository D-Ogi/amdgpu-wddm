// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3dkmthk.h>

namespace native12 {
struct MemoryPolicyKmt {
    PFND3DKMT_OPENADAPTERFROMLUID open{};
    PFND3DKMT_QUERYADAPTERINFO query{};
    PFND3DKMT_CLOSEADAPTER close{};
};
namespace memory_policy_detail {
inline HRESULT status_result(NTSTATUS status) noexcept {
    // These are synchronous calls. An informational success is not completion.
    return status==0?S_OK:status<0?HRESULT_FROM_NT(status):E_UNEXPECTED;
}
class AdapterHandle final {
    D3DKMT_HANDLE handle_{};
    PFND3DKMT_CLOSEADAPTER close_{};
    D3DKMT_HANDLE* unresolved_{};
    bool attempted_{};
    HRESULT result_{S_OK};
public:
    AdapterHandle(D3DKMT_HANDLE handle,PFND3DKMT_CLOSEADAPTER close,D3DKMT_HANDLE* unresolved) noexcept
        :handle_(handle),close_(close),unresolved_(unresolved) {}
    AdapterHandle(const AdapterHandle&)=delete;
    AdapterHandle& operator=(const AdapterHandle&)=delete;
    ~AdapterHandle(){if(!attempted_)(void)close();}
    HRESULT close() noexcept {
        if(attempted_)return result_;
        attempted_=true;
        if(!handle_)return S_OK;
        D3DKMT_CLOSEADAPTER args{};args.hAdapter=handle_;
        result_=status_result(close_(&args));
        if(result_==S_OK)handle_=0;
        else *unresolved_=handle_;
        // Failed ownership transfers to the caller. No destructor retry can
        // conceal the error or use callbacks after their lifetime ends.
        return result_;
    }
};
}
// A failed close returns its error ahead of a query error and transfers the
// unresolved handle to the required output. The caller must retain that owner.
// Read the OS-visible capability of physical adapter 0 on the already-admitted
// LUID. No device, context, queue, allocation or submission is created here.
// This is an explicit BC250 host IO-coherence policy input, independent of UMA
// and CacheCoherentUMA. It verifies an advertised capability, not hardware work.
// Sources: WDK26100 D3DKMT_QUERY_GPUMMU_CAPS / KMTQAITYPE_QUERY_GPUMMU_CAPS;
// KMD WddmGpuMmuCaps declares cache-coherent memory, backed by snooped system PTEs.
inline HRESULT query_memory_policy(const LUID& luid,const MemoryPolicyKmt& kmt,
                                   bool* io_coherent,D3DKMT_HANDLE* unresolved_adapter) noexcept {
    if(io_coherent)*io_coherent=false;
    if(unresolved_adapter)*unresolved_adapter=0;
    if(!io_coherent || !unresolved_adapter || !kmt.open || !kmt.query || !kmt.close)return E_INVALIDARG;
    static_assert(sizeof(D3DKMT_QUERY_GPUMMU_CAPS)==12);
    D3DKMT_OPENADAPTERFROMLUID opened{};opened.AdapterLuid=luid;
    NTSTATUS status=kmt.open(&opened);
    if(status<0)return memory_policy_detail::status_result(status);
    memory_policy_detail::AdapterHandle adapter(opened.hAdapter,kmt.close,unresolved_adapter);
    if(status!=0 || !opened.hAdapter){
        const HRESULT closed=adapter.close();
        return closed!=S_OK?closed:E_UNEXPECTED;
    }
    D3DKMT_QUERY_GPUMMU_CAPS caps{};caps.PhysicalAdapterIndex=0;
    D3DKMT_QUERYADAPTERINFO query{};query.hAdapter=opened.hAdapter;
    query.Type=KMTQAITYPE_QUERY_GPUMMU_CAPS;
    query.pPrivateDriverData=&caps;query.PrivateDriverDataSize=sizeof(caps);
    HRESULT result=memory_policy_detail::status_result(kmt.query(&query));
    if(result==S_OK && (caps.PhysicalAdapterIndex!=0 || caps.Caps.Flags.Reserved ||
       !caps.Caps.VirtualAddressBitCount || caps.Caps.VirtualAddressBitCount>64))result=E_UNEXPECTED;
    const HRESULT closed=adapter.close();
    if(closed!=S_OK)return closed;
    if(result!=S_OK)return result;
    *io_coherent=caps.Caps.Flags.CacheCoherentMemorySupported!=0;
    return S_OK;
}
}
