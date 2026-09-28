// SPDX-License-Identifier: MIT
#include "queue-request.h"
#include <cassert>
#include <cstring>
#include <cstdio>
static unsigned calls;
static HRESULT APIENTRY create(D3D12DDI_HRTCOMMANDQUEUE,D3DDDICB_CREATECONTEXTVIRTUAL* a) {
    ++calls;
    assert(a->NodeOrdinal==0 && a->EngineAffinity==1 && a->Flags.Value==0);
    assert(a->PrivateDriverDataSize==BC250_UMD_CONTEXT_SIZE_V2);
    const auto& b=*static_cast<const bc250_umd_context_private*>(a->pPrivateDriverData);
    assert(b.magic==BC250_UMD_CONTEXT_MAGIC && b.version==2 && b.size==sizeof(b));
    assert(b.ip_type==AMDGPU_HW_IP_GFX && b.node_ordinal==a->NodeOrdinal);
    assert(!b.flags && !b.ip_instance && !b.ring && !b.priority && !b.stable_pstate);
    for(auto v:b.reserved) assert(!v);
    for(auto v:b.reserved_v2) assert(!v);
    a->hContext=reinterpret_cast<HANDLE>(UINT_PTR(1));return S_OK;
}
static HRESULT APIENTRY destroy(D3D12DDI_HRTCOMMANDQUEUE,const D3DDDICB_DESTROYCONTEXT*) {return S_OK;}
int main() {
    native12::ContextRequest request;
    D3D12DDIARG_CREATECOMMANDQUEUE_0050 q{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 cb{};
    cb.pfnCreateContextVirtualCb=create;cb.pfnDestroyContextCb=destroy;
    native12::QueueContext context({},cb);
    for(unsigned flags=1;flags<=7;flags++) {
        q.QueueFlags=static_cast<D3D12DDI_COMMAND_QUEUE_FLAGS>(flags);
        assert(request.prepare(q)==S_OK);
        assert(context.open(request.args)==S_OK && context.close()==S_OK);
    }
    assert(calls==7);
    q.NodeMask=2;assert(request.prepare(q)==E_NOTIMPL && !request.args.pPrivateDriverData);
    q.NodeMask=1;q.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_PAGING;assert(request.prepare(q)==E_NOTIMPL);
    q.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_NONE;assert(request.prepare(q)==E_NOTIMPL);
    q.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;assert(request.prepare(q)==S_OK);
    puts("D3D12 queue KMD request tests passed");
}
