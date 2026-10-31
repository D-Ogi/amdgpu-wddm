// SPDX-License-Identifier: MIT
// Application-specific driver state (DDI 0113; DirectX-Specs Application_Specific_Driver_State_v0_07.md): the
// blob that a capture tool such as PIX reads from a device with GetApplicationSpecificDriverState and gives back to
// the runtime at replay with SetApplicationSpecificDriverState. The runtime then hands it to the driver through
// QueryAdapterInfoCb2 (D3DDDI_QUERYADAPTERTYPE_APPLICATIONSPECIFICDRIVERBLOB), and the replay device behaves as the
// device of the captured application. Design: docs/design/app-specific-driver-state.md.
//
// The state of this driver is the profile that the device applies: the per-application settings of the control
// application (app-settings-core.h, the values and their sources, with the image name they were read for) and the
// engine profile, the vkd3d-proton config flags that the engine selected for the program. The engine part stays
// empty until the engine reports its flags (EngineProfile::valid). This build cannot give flags to the engine
// (kEngineProfileApplied), so it does not use a blob whose engine part is valid: USED means the device applies all
// of the blob.
//
// The blob is the DDI header D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 and then the payload, all
// little-endian, with no padding:
//   0   GUID   kStateGuid
//   16  UINT32 kStateFormat
//   20  UINT32 0
//   24  UINT64 the file version of this shell (VS_FIXEDFILEINFO, 0 for a build without a version resource)
//   32  UINT32 the payload bytes after this field
//   36  UINT32 EngineProfile::valid (0 or 1)
//   40  UINT64 EngineProfile::config_flags (0 when not valid)
//   48  UINT32 kCount, the number of settings
//   52  kCount x {UINT32 value, UINT32 Source}
//   ..  UINT32 n, the UTF-16 units of the image name (0 to MAX_PATH-1), then n UTF-16 units without a terminator
// The 16 bytes after the GUID are the versioning data of the identifier. A blob is this driver's own only when the
// GUID and all 16 bytes are equal to the identifier of this build: config flag bits and setting meanings belong to
// one build, so a blob of another build is never applied.
//
// Everything here is free of the shell's device state, so app-state-test.cpp checks it with no runtime.
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <cstdint>
#include <cstring>
#include <new>
#include "../app-settings/app-settings-core.h"

#if defined(D3D12DDI_BUILD_VERSION_0119)
namespace native12::app_state {
namespace as = amdgpu_wddm::app_settings;

// {99b14533-68f3-4be0-b0f8-1d5104d43f30}: this driver's application-specific driver state.
inline constexpr GUID kStateGuid = {0x99b14533, 0x68f3, 0x4be0, {0xb0, 0xf8, 0x1d, 0x51, 0x04, 0xd4, 0x3f, 0x30}};
inline constexpr uint32_t kStateFormat = 1;
inline constexpr size_t kIdentifierBytes = sizeof(D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054);
static_assert(kIdentifierBytes == 32, "GUID and 16 bytes of versioning data");
inline constexpr size_t kFixedBytes = kIdentifierBytes + 4 + 4 + 8 + 4 + as::kCount * 8 + 4;
inline constexpr size_t kMaxNameUnits = MAX_PATH - 1;
inline constexpr size_t kMaxBytes = kFixedBytes + kMaxNameUnits * 2;
// The largest blob the device reads from the runtime. A blob above it is not this driver's, so it is ignored
// without a copy.
inline constexpr UINT kReadLimit = 64u * 1024u;

// The vkd3d-proton config flags of the program, as the engine selected them (its application_override table and
// vkd3d-proton.conf). valid is false while the engine does not report them.
struct EngineProfile {
    bool valid = false;
    uint64_t config_flags = 0;
};
// True when the device gives the engine part of a used blob to the engine. The engine ABI 1.10 has no entry that
// reports or sets the config flags, so this build writes no valid engine part and uses no blob that has one.
inline constexpr bool kEngineProfileApplied = false;
struct Record {
    as::Settings settings;   // values, sources and the image name; the rejections are not part of the state
    EngineProfile engine;
};

inline D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 identifier(uint64_t driver_version) noexcept {
    D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 id{};
    id.DriverOpaqueGUID = kStateGuid;
    const uint32_t format = kStateFormat, reserved = 0;
    std::memcpy(id.DriverOpaqueVersioningData, &format, 4);
    std::memcpy(id.DriverOpaqueVersioningData + 4, &reserved, 4);
    std::memcpy(id.DriverOpaqueVersioningData + 8, &driver_version, 8);
    return id;
}

// CheckDriverMatchingIdentifier for D3D12DDI_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE: COMPATIBLE for the
// identifier of this build, INCOMPATIBLE_VERSION for this driver's GUID with other versioning data, and
// UNRECOGNIZED for any other GUID or no identifier.
inline D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS check(
    const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054* given, uint64_t driver_version) noexcept {
    if (!given || std::memcmp(&given->DriverOpaqueGUID, &kStateGuid, sizeof(GUID)))
        return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
    const auto own = identifier(driver_version);
    return std::memcmp(given->DriverOpaqueVersioningData, own.DriverOpaqueVersioningData, 16)
        ? D3D12DDI_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION
        : D3D12DDI_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
}

inline size_t name_units(const as::Settings& s) noexcept {
    size_t n = 0;
    while (n < kMaxNameUnits && s.application[n]) ++n;
    return n;
}
inline size_t encoded_size(const Record& r) noexcept { return kFixedBytes + name_units(r.settings) * 2; }

namespace detail {
inline void put32(uint8_t*& p, uint32_t v) noexcept { std::memcpy(p, &v, 4); p += 4; }
inline void put64(uint8_t*& p, uint64_t v) noexcept { std::memcpy(p, &v, 8); p += 8; }
inline uint32_t get32(const uint8_t*& p) noexcept { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; }
inline uint64_t get64(const uint8_t*& p) noexcept { uint64_t v; std::memcpy(&v, p, 8); p += 8; return v; }
} // namespace detail

// Writes the blob of r into out. Returns the bytes written, or 0 when capacity is too small (nothing written then).
inline size_t encode(const Record& r, uint64_t driver_version, void* out, size_t capacity) noexcept {
    const size_t size = encoded_size(r);
    if (!out || capacity < size) return 0;
    auto* p = static_cast<uint8_t*>(out);
    const auto id = identifier(driver_version);
    std::memcpy(p, &id, kIdentifierBytes); p += kIdentifierBytes;
    detail::put32(p, static_cast<uint32_t>(size - kIdentifierBytes - 4));
    const bool engine = r.engine.valid && kEngineProfileApplied;
    detail::put32(p, engine ? 1u : 0u);
    detail::put64(p, engine ? r.engine.config_flags : 0u);
    detail::put32(p, as::kCount);
    for (unsigned i = 0; i < as::kCount; ++i) {
        const as::Value& v = r.settings.values[i];
        detail::put32(p, v.set() ? v.value : 0u);
        detail::put32(p, static_cast<uint32_t>(v.source));
    }
    const size_t n = name_units(r.settings);
    detail::put32(p, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const uint16_t unit = static_cast<uint16_t>(r.settings.application[i]);
        std::memcpy(p, &unit, 2); p += 2;
    }
    return size;
}

// Reads a blob. True only for a blob of this build whose every field is one this driver writes: then out holds the
// record. Any other blob leaves out unchanged and returns false.
inline bool decode(const void* data, size_t size, uint64_t driver_version, Record& out) noexcept {
    if (!data || size < kFixedBytes) return false;
    D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 id;
    std::memcpy(&id, data, kIdentifierBytes);
    if (check(&id, driver_version) != D3D12DDI_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data) + kIdentifierBytes;
    if (detail::get32(p) != size - kIdentifierBytes - 4) return false;
    Record r;
    const uint32_t valid = detail::get32(p);
    const uint64_t flags = detail::get64(p);
    if (valid > 1 || (!valid && flags) || (valid && !kEngineProfileApplied)) return false;
    r.engine.valid = valid == 1; r.engine.config_flags = flags;
    if (detail::get32(p) != as::kCount) return false;
    for (unsigned i = 0; i < as::kCount; ++i) {
        const uint32_t value = detail::get32(p), source = detail::get32(p);
        if (source > static_cast<uint32_t>(as::Source::Profile)) return false;
        const auto src = static_cast<as::Source>(source);
        if (src == as::Source::None ? value != 0 : !as::valid(static_cast<as::Setting>(i), value)) return false;
        r.settings.values[i].value = value; r.settings.values[i].source = src;
    }
    const uint32_t n = detail::get32(p);
    if (n > kMaxNameUnits || size != kFixedBytes + size_t{n} * 2) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t unit; std::memcpy(&unit, p, 2); p += 2;
        // An image name is a file name: no terminator inside it and no directory separator (app-settings-core.h,
        // system_registry, refuses such a name too).
        if (!unit || unit == L'\\' || unit == L'/') return false;
        r.settings.application[i] = static_cast<wchar_t>(unit);
    }
    r.settings.application[n] = 0;
    out = r;
    return true;
}

// What CreateDevice found. status is the answer of pfnGetApplicationSpecificDriverBlobStatus. record is valid when
// status is USED. query is the HRESULT of the last QueryAdapterInfoCb2 call (S_OK when none was made), size the blob
// size the runtime named.
struct Found {
    D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_STATUS status = D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED;
    HRESULT query = S_OK;
    UINT size = 0;
    Record record;
};

// Asks the runtime for the blob of this adapter (Application_Specific_Driver_State_v0_07.md, "D3DDDI_QUERYADAPTERTYPE"):
//   no QueryAdapterInfoCb2, E_INVALIDARG (a runtime without the query), D3DDDIERR_NOTAVAILABLE (no blob): NOT_SPECIFIED;
//   S_OK: USED for this build's blob, IGNORED for any other;
//   E_NOT_SUFFICIENT_BUFFER: the size is in PrivateDriverDataSize; one more call with that size, or IGNORED when the
//   size is above kReadLimit or the memory is not there;
//   any other failure: UNKNOWN, since the driver cannot tell whether a blob was set.
// query is a callable HRESULT(D3DDDICB_QUERYADAPTERINFO2*), null when the runtime gave no QueryAdapterInfoCb2.
template<class Query> Found read(Query* query, uint64_t driver_version) noexcept {
    Found f;
    if (!query) return f;
    alignas(8) uint8_t local[kMaxBytes + 64];
    D3DDDICB_QUERYADAPTERINFO2 q{};
    q.QueryType = D3DDDI_QUERYADAPTERTYPE_APPLICATIONSPECIFICDRIVERBLOB;
    q.pPrivateDriverData = local; q.PrivateDriverDataSize = sizeof(local);
    f.query = (*query)(&q);
    uint8_t* heap = nullptr;
    if (f.query == E_NOT_SUFFICIENT_BUFFER) {
        f.size = q.PrivateDriverDataSize;
        if (f.size <= sizeof(local) || f.size > kReadLimit || !(heap = new (std::nothrow) uint8_t[f.size])) {
            f.status = D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED;
            return f;
        }
        q.pPrivateDriverData = heap; q.PrivateDriverDataSize = f.size;
        f.query = (*query)(&q);
    }
    if (f.query == S_OK) {
        f.size = q.PrivateDriverDataSize;
        const size_t given = f.size <= (heap ? size_t{f.size} : sizeof(local)) ? f.size : 0;
        f.status = given && decode(q.pPrivateDriverData, given, driver_version, f.record)
            ? D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_USED : D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED;
    } else if (f.query == E_INVALIDARG || f.query == D3DDDIERR_NOTAVAILABLE) {
        f.status = D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED;
    } else {
        f.status = D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_UNKNOWN;
    }
    delete[] heap;
    return f;
}

inline const char* status_name(D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_STATUS s) noexcept {
    switch (s) {
    case D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_USED: return "used";
    case D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED: return "ignored";
    case D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED: return "not-specified";
    default: return "unknown";
    }
}

// The file version of the module that holds this code (VS_FIXEDFILEINFO dwFileVersionMS and LS), or 0 when the module
// has no version resource. The release stamps the version after the build (tools/build/build-umd-d3d12.ps1, BD-166),
// so the value is read from the image and not compiled in.
inline uint64_t module_file_version() noexcept {
    static const uint64_t version = []() noexcept -> uint64_t {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&kStateGuid), &module))
            return 0;
        HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));   // VS_VERSION_INFO, RT_VERSION
        if (!info) return 0;
        HGLOBAL loaded = LoadResource(module, info);
        const auto* bytes = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
        const DWORD size = SizeofResource(module, info);
        if (!bytes) return 0;
        // VS_FIXEDFILEINFO starts with the signature 0xFEEF04BD on a 32-bit boundary of the resource.
        for (DWORD at = 0; at + 52 <= size; at += 4) {
            uint32_t signature; std::memcpy(&signature, bytes + at, 4);
            if (signature != 0xFEEF04BDu) continue;
            uint32_t ms, ls; std::memcpy(&ms, bytes + at + 8, 4); std::memcpy(&ls, bytes + at + 12, 4);
            return (uint64_t{ms} << 32) | ls;
        }
        return 0;
    }();
    return version;
}

} // namespace native12::app_state
#endif
