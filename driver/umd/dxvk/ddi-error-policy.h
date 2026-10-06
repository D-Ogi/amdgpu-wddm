// SPDX-License-Identifier: MIT
#pragma once
#include <d3d10umddi.h>
#include <winerror.h>
namespace bc250::umd {
// A DDI entry with a void return reports a failure through pfnSetErrorCb. Each entry's reference page
// names the statuses the runtime accepts from that entry. A status outside that set is not an error
// report: the runtime treats it as a driver bug, logs it and removes the device on purpose
// (ref/windows-driver-docs/.../display/handling-errors.md). The class below is that allowed set, and
// every report goes through ddi_class_status, so an entry can only report what its page allows.
//
// The per-entry class table lives next to the gate that enforces it: tools/quality/ddi_error_policy.py
// carries the entry name, the class and the reference page that states the set.
// AllowDeviceRemoved is the baseline of handling-errors.md: every entry may report device removal,
// and only a capability-check entry may not ("The driver cannot return D3DDDIERR_DEVICEREMOVED for
// any check-type function"). The classes below therefore split in two: the ordinary classes, which
// carry device removal next to whatever their own page names, and the `check_` classes, which carry
// their page's statuses and never device removal.
enum class DdiErrorClass {
    // AllowDeviceRemoved. Set, clear, copy, destroy, draw, flush and tile data entries.
    removed_only,
    // AllowOutOfMemory. Every Create* entry except CreateResource and CreateQuery.
    out_of_memory,
    // CreateResource and the DXGI resource entries: + DXGI_DDI_ERR_UNSUPPORTED for a request the
    // driver cannot serve. The runtime fails the caller instead of removing the device.
    unsupported,
    // CreateQuery: + DXGI_DDI_ERR_NONEXCLUSIVE, because counters are exclusive.
    non_exclusive,
    // AllowMapErrors and AllowGetDataErrors: + DXGI_DDI_ERR_WASSTILLDRAWING.
    still_drawing,
    // UpdateTileMappings, CopyTileMappings and GetMipPacking: + E_INVALIDARG for a missing argument.
    // None of the three is a check-type entry, so a lost device still reports removal.
    invalid_arg,
    // ResizeTilePool: + E_INVALIDARG for the size and E_OUTOFMEMORY for the page tables.
    invalid_arg_oom,
    // GenMips: + E_FAIL for the resource flags and E_INVALIDARG for the MIP type.
    fail_or_invalid_arg,
    // CheckMultisampleQualityLevels: E_INVALIDARG only. A check-type entry answers after the device
    // is gone, so device removal is not among its statuses.
    check_invalid_arg,
    // CheckFormatSupport: E_FAIL and E_INVALIDARG.
    check_fail_or_invalid_arg,
    // CheckCounter: DXGI_DDI_ERR_UNSUPPORTED for a well-known counter, E_INVALIDARG for the
    // arguments of a device-dependent one.
    check_unsupported,
    // CheckCounterInfo, CheckDeferredContextHandleSizes and the other capability answers: no status.
    nothing,
};
// API HRESULT values are not the DDI values, even for the same condition.
constexpr HRESULT ddi_device_status(HRESULT hr) {
    switch (hr) {
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DEVICE_HUNG:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return D3DDDIERR_DEVICEREMOVED;
    default: return hr;
    }
}
constexpr bool ddi_status_allowed(DdiErrorClass policy,HRESULT status) {
    switch (policy) {
    case DdiErrorClass::removed_only: return status==D3DDDIERR_DEVICEREMOVED;
    case DdiErrorClass::out_of_memory: return status==D3DDDIERR_DEVICEREMOVED || status==E_OUTOFMEMORY;
    case DdiErrorClass::unsupported:
        return status==D3DDDIERR_DEVICEREMOVED || status==E_OUTOFMEMORY || status==DXGI_DDI_ERR_UNSUPPORTED;
    case DdiErrorClass::non_exclusive:
        return status==D3DDDIERR_DEVICEREMOVED || status==E_OUTOFMEMORY || status==DXGI_DDI_ERR_NONEXCLUSIVE;
    case DdiErrorClass::still_drawing:
        return status==D3DDDIERR_DEVICEREMOVED || status==DXGI_DDI_ERR_WASSTILLDRAWING;
    case DdiErrorClass::invalid_arg: return status==D3DDDIERR_DEVICEREMOVED || status==E_INVALIDARG;
    case DdiErrorClass::invalid_arg_oom:
        return status==D3DDDIERR_DEVICEREMOVED || status==E_INVALIDARG || status==E_OUTOFMEMORY;
    case DdiErrorClass::fail_or_invalid_arg:
        return status==D3DDDIERR_DEVICEREMOVED || status==E_FAIL || status==E_INVALIDARG;
    case DdiErrorClass::check_invalid_arg: return status==E_INVALIDARG;
    case DdiErrorClass::check_fail_or_invalid_arg: return status==E_FAIL || status==E_INVALIDARG;
    case DdiErrorClass::check_unsupported: return status==E_INVALIDARG || status==DXGI_DDI_ERR_UNSUPPORTED;
    case DdiErrorClass::nothing: return false;
    }
    return false;
}
// True for the classes that may carry device removal. The other classes belong to check-type and tile
// entries, which the runtime calls even after the device is gone.
constexpr bool ddi_class_reports_removal(DdiErrorClass policy) {
    return ddi_status_allowed(policy,D3DDDIERR_DEVICEREMOVED);
}
// The status for a failure this entry cannot describe: the strongest one its page allows. Device
// removal where the page allows it, and the page's own general failure otherwise. S_OK means the page
// allows no status at all, so the entry reports nothing and answers with its empty result.
constexpr HRESULT ddi_class_internal_failure(DdiErrorClass policy) {
    switch (policy) {
    case DdiErrorClass::removed_only:
    case DdiErrorClass::out_of_memory:
    case DdiErrorClass::unsupported:
    case DdiErrorClass::non_exclusive:
    case DdiErrorClass::still_drawing:
    case DdiErrorClass::invalid_arg:
    case DdiErrorClass::invalid_arg_oom:
    case DdiErrorClass::fail_or_invalid_arg: return D3DDDIERR_DEVICEREMOVED;
    case DdiErrorClass::check_invalid_arg: return E_INVALIDARG;
    case DdiErrorClass::check_fail_or_invalid_arg: return E_FAIL;
    case DdiErrorClass::check_unsupported: return DXGI_DDI_ERR_UNSUPPORTED;
    case DdiErrorClass::nothing: return S_OK;
    }
    return S_OK;
}
// The status this entry may carry for `raw`, or S_OK when the entry may report nothing at all.
constexpr HRESULT ddi_class_status(DdiErrorClass policy,HRESULT raw) {
    const HRESULT mapped=ddi_device_status(raw);
    if (ddi_status_allowed(policy,mapped)) return mapped;
    return ddi_class_internal_failure(policy);
}
// The class of the DDI entry this thread is inside. enter_context sets it and restores it, so the one
// report that happens outside an entry's own code can still obey the entry's page: the host bridge
// detects a lost device deep in the submission path and reports it from there. A thread that is in no
// entry keeps the baseline class, which carries device removal.
inline thread_local DdiErrorClass ddi_entry_class=DdiErrorClass::removed_only;
// Saves and restores the entry class across one DDI entry, so a nested call cannot leak its class.
struct DdiEntryClassScope {
    explicit DdiEntryClassScope(DdiErrorClass policy) noexcept : previous(ddi_entry_class) { ddi_entry_class=policy; }
    ~DdiEntryClassScope() { ddi_entry_class=previous; }
    DdiEntryClassScope(const DdiEntryClassScope &)=delete;
    DdiEntryClassScope &operator=(const DdiEntryClassScope &)=delete;
    DdiErrorClass previous;
};
}
