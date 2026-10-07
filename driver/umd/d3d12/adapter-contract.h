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
    // M15.14 increment 2: the kernel driver's start-latched answer about a client scan-out flip, from the
    // optional trailer behind the adapter identity. Every field zero is the correct reading of three
    // states that must behave alike - a kernel driver older than the trailer, a start whose operator
    // switch is off, and a query that wrote less than the whole trailer - and in all three this shell
    // must behave as 0.7.207.1 did: no scan-out request leaves it.
    bc250_scanout_caps scanout{};
    UINT64 luid{};
};
// The trailer of a zero-initialized query buffer, or a zeroed structure. No success flag is needed: the
// caller tests flags, which is zero unless every header field was right. post_width and post_height are
// the geometry of the source mode the kernel driver admits a flip at, whatever mode that is: the shell
// compares a chain against them and never against a size of its own.
inline bc250_scanout_caps decode_scanout_caps(const unsigned char* data,size_t bytes) noexcept {
    bc250_scanout_caps caps{};
    if(!data || bytes<BC250_SCANOUT_CAPS_TOTAL)return {};
    std::memcpy(&caps,data+BC250_SCANOUT_CAPS_OFFSET,sizeof(caps));
    if(caps.magic!=BC250_SCANOUT_CAPS_MAGIC || caps.version!=BC250_SCANOUT_CAPS_VERSION ||
       caps.size!=sizeof(caps) || !caps.post_width || !caps.post_height)return {};
    return caps;
}
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
    // The scan-out trailer is optional and never a reason to refuse the adapter: a driver that wrote
    // none leaves the tail of this zero-initialized buffer alone and the shell keeps its old behaviour.
    candidate.scanout=decode_scanout_caps(data,sizeof(data));
    output=candidate;return S_OK;
}
}
