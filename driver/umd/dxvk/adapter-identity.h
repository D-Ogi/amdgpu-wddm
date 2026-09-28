// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "../../contract/bc250_adapter_identity.h"
#include <cstring>
namespace bc250::umd {
// Adapter callbacks exist before a device RuntimeDomain. No Vulkan objects or
// device callbacks are required to query the OS-assigned adapter identity.
inline HRESULT query_adapter_identity(HANDLE adapter,PFND3DDDI_QUERYADAPTERINFOCB query,UINT64 &luid) {
    if (!adapter || !query) return E_INVALIDARG;
    unsigned char data[BC250_ADAPTER_CAPS_BYTES]{};
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
}
