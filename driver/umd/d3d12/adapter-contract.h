// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <cstring>
#include "../../contract/bc250_umd_private.h"
#include "../../contract/bc250_adapter_identity.h"
#include "../../contract/bc250_scanout_caps.h"
namespace native12 {
struct AdapterContract {
    bc250_umd_private caps{};
    UINT64 luid{};
};
inline HRESULT query_contract(D3D12DDI_HRTADAPTER adapter,
                              PFND3DDDI_QUERYADAPTERINFOCB query,AdapterContract& output) noexcept {
    output={};
    if(!adapter.handle || !query)return E_INVALIDARG;
    static_assert(sizeof(bc250_umd_private)==BC250_ADAPTER_IDENTITY_OFFSET);
    // Size the buffer from the LAST trailer the contract defines, not from the last one this function reads.
    // The kernel driver writes an optional trailer only when the whole of it fits in the query, so a buffer
    // sized to an earlier trailer makes every later one unreachable, and unreachable reads as zeros - a
    // reader then answers "not supported" forever with nothing in any log to say why.
    unsigned char data[BC250_SCANOUT_CAPS_TOTAL]{};
    static_assert(sizeof(data)>=BC250_SCANOUT_CAPS_TOTAL,
                  "the adapter query buffer must cover every trailer the kernel driver may write");
    static_assert(BC250_SCANOUT_CAPS_OFFSET>=BC250_ADAPTER_CAPS_BYTES,
                  "a new trailer overlaps the caps blob or the adapter identity this function reads");
    D3DDDICB_QUERYADAPTERINFO request{};
    request.pPrivateDriverData=data;request.PrivateDriverDataSize=sizeof(data);
    HRESULT hr=query(adapter.handle,&request);if(FAILED(hr))return hr;
    AdapterContract candidate;bc250_adapter_identity identity{};
    std::memcpy(&candidate.caps,data,sizeof(candidate.caps));
    std::memcpy(&identity,data+BC250_ADAPTER_IDENTITY_OFFSET,sizeof(identity));
    const auto& caps=candidate.caps;
    if(caps.magic!=BC250_UMD_PRIVATE_MAGIC || caps.version!=BC250_UMD_PRIVATE_VERSION ||
       caps.size!=sizeof(caps) || (caps.flags&BC250_UMD_F_UNMEASURED) ||
       !(caps.submittable_node_mask&1u) || !(caps.hw_ip_mask&(1u<<AMDGPU_HW_IP_GFX)) ||
       !caps.hw_ip[AMDGPU_HW_IP_GFX].available_rings ||
       identity.magic!=BC250_ADAPTER_IDENTITY_MAGIC ||
       identity.version!=BC250_ADAPTER_IDENTITY_VERSION || identity.size!=sizeof(identity) ||
       identity.reserved)return E_NOINTERFACE;
    candidate.luid=(UINT64(identity.luid_high)<<32)|identity.luid_low;
    output=candidate;return S_OK;
}
}
