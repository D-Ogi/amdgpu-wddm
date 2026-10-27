// SPDX-License-Identifier: MIT
#pragma once
#include "bc250_vkd3d_engine.h"

namespace engine_ddi::partial {
// One engine reference, shared between collections through the owning Program.
// The engine module must outlive the collection, as for ordinary engine PSOs.
class Library final {
    void* handle_ = nullptr;
    void (APIENTRY *release_)(void*) = nullptr;
public:
    Library() = default;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    ~Library() { if (handle_) release_(handle_); }
    void* get() const noexcept { return handle_; }
    HRESULT create(const BC250_VKD3D_ENGINE_FUNCS* funcs, void* device,
                   const BC250_VKD3D_PARTIAL_GRAPHICS_DESC& desc) noexcept {
        // Check Size before reading any appended entry. A reused owner must not
        // replace a library which another imported collection still references.
        if (handle_) return E_UNEXPECTED;
        if (!funcs || funcs->Size < sizeof(*funcs) ||
            (funcs->AbiVersion >> 16) != BC250_VKD3D_ENGINE_ABI_MAJOR ||
            (funcs->AbiVersion & 0xffffu) < 4 ||
            !funcs->CreatePartialGraphicsLibrary || !funcs->ReleasePartialGraphicsLibrary)
            return E_NOINTERFACE;
        void* result = nullptr;
        const HRESULT hr = funcs->CreatePartialGraphicsLibrary(device, &desc, &result);
        if (FAILED(hr)) return hr;
        if (!result) return E_UNEXPECTED;
        release_ = funcs->ReleasePartialGraphicsLibrary;
        handle_ = result;
        return hr;
    }
};
}
