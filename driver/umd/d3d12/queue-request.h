// SPDX-License-Identifier: MIT
#pragma once
#include "queue-context.h"
#include <cstring>
#include "../../contract/bc250_umd_submit.h"
namespace native12 {
// The WDDM node of bc250kmd's compute ring (docs/design/compute-node.md): one MEC queue.
constexpr unsigned kComputeNode=2;
// Owned storage keeps pPrivateDriverData valid throughout the synchronous callback.
// A DIRECT queue runs on GFX, node 0. A COMPUTE or COPY queue runs where the hosted ICD binds the
// engine's VkQueue for it: GFX on node 0, or COMPUTE on node 2 when the ICD shows a compute queue
// family (the engine then takes it for COMPUTE and COPY queues). Its context is therefore opened at
// that bind, with one of these two requests (QueueEngineRegistry::bind_context).
struct ContextRequest final {
    bc250_umd_context_private blob{};
    D3DDDICB_CREATECONTEXTVIRTUAL args{};
    ContextRequest()=default;
    ContextRequest(const ContextRequest&)=delete;
    ContextRequest& operator=(const ContextRequest&)=delete;
    static bool opened_at_bind(const D3D12DDIARG_CREATECOMMANDQUEUE_0050& queue) noexcept {
        return !(static_cast<unsigned>(queue.QueueFlags) & D3D12DDI_COMMAND_QUEUE_FLAG_3D);
    }
    // compute: the COMPUTE request on node 2, for a COMPUTE or COPY queue only.
    HRESULT prepare(const D3D12DDIARG_CREATECOMMANDQUEUE_0050& queue,bool compute=false) noexcept {
        blob={};args={};
        const unsigned flags=static_cast<unsigned>(queue.QueueFlags);
        constexpr unsigned allowed=D3D12DDI_COMMAND_QUEUE_FLAG_3D |
            D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE | D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        if(!flags || (flags & ~allowed) || queue.NodeMask>1 ||
           queue.QueueCreationFlags!=0 || queue.SchedulingGroup.pDrvPrivate) return E_NOTIMPL;
        if(compute && !opened_at_bind(queue)) return E_NOTIMPL;
        blob.magic=BC250_UMD_CONTEXT_MAGIC;blob.version=BC250_UMD_CONTEXT_VERSION;
        blob.size=sizeof(blob);blob.ip_type=compute?AMDGPU_HW_IP_COMPUTE:AMDGPU_HW_IP_GFX;
        blob.node_ordinal=compute?kComputeNode:0;
        args.NodeOrdinal=blob.node_ordinal;args.EngineAffinity=1;
        args.pPrivateDriverData=&blob;args.PrivateDriverDataSize=sizeof(blob);
        return S_OK;
    }
    // A hosted ICD's CREATE_QUEUE_CONTEXT request (a D3DKMT_CREATECONTEXTVIRTUAL) equals this one: the
    // same node, engine affinity and BC2C bytes, no flags and no context yet. ClientHint is the ICD's and
    // is not compared. A template, so that this header needs no d3dkmthk.h.
    template<class KmtRequest> bool matches(const KmtRequest& a) const noexcept {
        return args.pPrivateDriverData && !a.hContext && a.NodeOrdinal==args.NodeOrdinal &&
            a.EngineAffinity==args.EngineAffinity && !a.Flags.Value && a.pPrivateDriverData &&
            a.PrivateDriverDataSize==sizeof(blob) && !std::memcmp(a.pPrivateDriverData,&blob,sizeof(blob));
    }
};
}
