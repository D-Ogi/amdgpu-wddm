// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "../../contract/bc250_adapter_identity.h"
#include "../../contract/bc250_scanout_caps.h"
#include <cstring>
namespace bc250::umd {
// Adapter callbacks exist before a device RuntimeDomain. No Vulkan objects or
// device callbacks are required to query the OS-assigned adapter identity.
inline HRESULT query_adapter_identity(HANDLE adapter,PFND3DDDI_QUERYADAPTERINFOCB query,UINT64 &luid) {
    if (!adapter || !query) return E_INVALIDARG;
    // Size the buffer from the LAST trailer the contract defines, not from the one this function reads.
    // The kernel driver writes an optional trailer only when the whole of it fits in the query, so a
    // buffer sized to an earlier trailer makes every later one unreachable, and unreachable reads as
    // zeros - a reader then answers "not supported" forever with nothing in any log to say why.
    unsigned char data[BC250_SCANOUT_CAPS_TOTAL]{};
    static_assert(sizeof(data)>=BC250_SCANOUT_CAPS_TOTAL,
                  "the adapter query buffer must cover every trailer the kernel driver may write");
    static_assert(BC250_SCANOUT_CAPS_OFFSET>=BC250_ADAPTER_CAPS_BYTES,
                  "a new trailer overlaps the adapter identity this function reads");
    D3DDDICB_QUERYADAPTERINFO request{};
    request.pPrivateDriverData=data; request.PrivateDriverDataSize=sizeof(data);
    const HRESULT hr=query(adapter,&request);
    if (FAILED(hr)) return hr;
    bc250_adapter_identity identity{};
    std::memcpy(&identity,data+BC250_ADAPTER_IDENTITY_OFFSET,sizeof(identity));
    if (identity.magic!=BC250_ADAPTER_IDENTITY_MAGIC ||
        identity.version!=BC250_ADAPTER_IDENTITY_VERSION ||
        identity.size!=sizeof(identity) || identity.reserved) return DXGI_ERROR_UNSUPPORTED;
    luid=(UINT64(identity.luid_high)<<32)|identity.luid_low;
    return S_OK;
}
// M15.14 increment 3: the scan-out caps trailer of a zero-initialized query buffer, or a zeroed structure.
// The caller tests flags, which is zero unless every header field was right; post_width and post_height are
// the geometry of the source mode the kernel driver admits a flip at now. The same decode as the D3D12
// shell's (driver/umd/d3d12/adapter-contract.h) and the router front's.
inline bc250_scanout_caps decode_scanout_caps(const unsigned char *data,size_t bytes) noexcept {
    bc250_scanout_caps caps{};
    if (!data || bytes<BC250_SCANOUT_CAPS_TOTAL) return {};
    std::memcpy(&caps,data+BC250_SCANOUT_CAPS_OFFSET,sizeof(caps));
    if (caps.magic!=BC250_SCANOUT_CAPS_MAGIC || caps.version!=BC250_SCANOUT_CAPS_VERSION ||
        caps.size!=sizeof(caps) || !caps.post_width || !caps.post_height) return {};
    return caps;
}
// The trailer of this moment. A kernel driver that commits display modes answers each query with the
// source mode of that moment, so the shell asks again for every primary it creates instead of keeping the
// copy of the adapter's open. A failed query is no trailer, which keeps the composed primary.
inline bc250_scanout_caps query_scanout_caps(HANDLE adapter,PFND3DDDI_QUERYADAPTERINFOCB query) noexcept {
    if (!adapter || !query) return {};
    unsigned char data[BC250_SCANOUT_CAPS_TOTAL]{};
    D3DDDICB_QUERYADAPTERINFO request{};
    request.pPrivateDriverData=data; request.PrivateDriverDataSize=sizeof(data);
    if (FAILED(query(adapter,&request))) return {};
    return decode_scanout_caps(data,sizeof(data));
}
}
