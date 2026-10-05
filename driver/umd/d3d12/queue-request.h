// SPDX-License-Identifier: MIT
#pragma once
#include "queue-context.h"
#include "../../contract/bc250_umd_submit.h"
namespace native12 {
// Owned storage keeps pPrivateDriverData valid throughout the synchronous callback.
struct ContextRequest final {
    bc250_umd_context_private blob{};
    D3DDDICB_CREATECONTEXTVIRTUAL args{};
    ContextRequest()=default;
    ContextRequest(const ContextRequest&)=delete;
    ContextRequest& operator=(const ContextRequest&)=delete;
    HRESULT prepare(const D3D12DDIARG_CREATECOMMANDQUEUE_0050& queue) noexcept {
        blob={};args={};
        // Initial native path: one physical node, ordinary graphics/compute/copy queues.
        // All execute on GFX; do not pretend that an async compute engine exists.
        const unsigned flags=static_cast<unsigned>(queue.QueueFlags);
        constexpr unsigned allowed=D3D12DDI_COMMAND_QUEUE_FLAG_3D |
            D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE | D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        if(!flags || (flags & ~allowed) || queue.NodeMask>1 ||
           queue.QueueCreationFlags!=0 || queue.SchedulingGroup.pDrvPrivate) return E_NOTIMPL;
        blob.magic=BC250_UMD_CONTEXT_MAGIC;blob.version=BC250_UMD_CONTEXT_VERSION;
        blob.size=sizeof(blob);blob.ip_type=AMDGPU_HW_IP_GFX;blob.node_ordinal=0;
        args.NodeOrdinal=blob.node_ordinal;args.EngineAffinity=1;
        args.pPrivateDriverData=&blob;args.PrivateDriverDataSize=sizeof(blob);
        return S_OK;
    }
};
}
