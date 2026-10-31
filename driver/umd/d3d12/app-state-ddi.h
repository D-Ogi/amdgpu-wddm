// SPDX-License-Identifier: MIT
// The shell's DDI entries of the application-specific driver state (CORE 0113; app-state.h has the blob and
// docs/design/app-specific-driver-state.md the design): the three CORE 0113 slots, the type 1 answer of
// CheckDriverMatchingIdentifier, and the read of the runtime's blob at CreateDevice.
#pragma once
#include "app-state.h"
#include "device-state.h"
#include <atomic>
#include <new>

#if defined(D3D12DDI_BUILD_VERSION_0119)
namespace native12 {
namespace app_state_detail {
inline Device* device(D3D12DDI_HDEVICE h) noexcept { return static_cast<Device*>(h.pDrvPrivate); }
// The record that Get returns: the profile the device applies. For a used blob that is the blob's record, so that
// a capture of a replay gives the state of the captured application again.
inline app_state::Record applied_record(const Device& d) noexcept {
    app_state::Record r;
    r.settings = d.settings();
    if (d.app_state && d.app_state->used) {
        r.engine.valid = d.app_state->engine_valid;
        r.engine.config_flags = d.app_state->engine_flags;
    }
    // The rejections are diagnostics of this process's registry and environment, not state of the application.
    r.settings.rejection_count = 0;
    return r;
}
} // namespace app_state_detail

// The CheckDriverMatchingIdentifier of the engine (engine-ddi context.cpp, unrecognized_identifier). The shell
// answers type D3D12DDI_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE and gives every other type to it.
inline std::atomic<PFND3D12DDI_CHECK_DRIVER_MATCHING_IDENTIFIER_0054> engine_check_identifier{};

inline UINT APIENTRY app_state_blob_size(D3D12DDI_HDEVICE h) {
    const Device* d = app_state_detail::device(h);
    if (!d) return 0;
    return static_cast<UINT>(app_state::encoded_size(app_state_detail::applied_record(*d)));
}
// E_INVALIDARG for no device, no buffer or a buffer smaller than the size that app_state_blob_size gives.
inline HRESULT APIENTRY app_state_blob(D3D12DDI_HDEVICE h, void* data, UINT size) {
    const Device* d = app_state_detail::device(h);
    if (!d || !data) return E_INVALIDARG;
    return app_state::encode(app_state_detail::applied_record(*d), app_state::module_file_version(), data, size)
        ? S_OK : E_INVALIDARG;
}
inline D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_STATUS APIENTRY app_state_blob_status(D3D12DDI_HDEVICE h) {
    const Device* d = app_state_detail::device(h);
    if (!d || !d->app_state) return D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_UNKNOWN;
    return static_cast<D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_STATUS>(d->app_state->blob_status);
}
inline D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY check_driver_matching_identifier(D3D12DDI_HDEVICE h,
    D3D12DDI_SERIALIZED_DATA_TYPE type, const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054* id) {
    if (type == D3D12DDI_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE)
        return app_state::check(id, app_state::module_file_version());
    const auto engine = engine_check_identifier.load(std::memory_order_acquire);
    return engine ? engine(h, type, id) : D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
}

// CreateDevice of a CORE 0113 or later device: asks the runtime for the blob of this adapter and records what it
// found. Returns false only when there is no memory for the record; the device then fails with E_OUTOFMEMORY.
// query is the adapter's QueryAdapterInfoCb2 with its adapter handle bound (null when the runtime gave none).
template<class Query> bool open_app_state(Device& device, Query* query, app_state::Found* found = nullptr) noexcept {
    auto state = std::unique_ptr<DeviceAppState>(new (std::nothrow) DeviceAppState{});
    if (!state) return false;
    const app_state::Found f = app_state::read(query, app_state::module_file_version());
    state->blob_status = static_cast<UINT>(f.status);
    if (f.status == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_USED) {
        state->used = true;
        state->settings = f.record.settings;
        state->engine_valid = f.record.engine.valid;
        state->engine_flags = f.record.engine.config_flags;
    }
    device.app_state = std::move(state);
    if (found) *found = f;
    return true;
}
} // namespace native12
#endif
