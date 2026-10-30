// SPDX-License-Identifier: MIT
// Host test of the application-specific driver state (app-state.h, app-state-ddi.h): the blob format, the type 1
// answer of CheckDriverMatchingIdentifier, and the read at CreateDevice against a runtime double that follows
// Application_Specific_Driver_State_v0_07.md ("D3DDDI_QUERYADAPTERTYPE"). It covers the conformance list of that
// spec ("Conformance Tests") that a driver can meet without the runtime: Get gives a blob that
// CheckDriverMatchingIdentifier accepts, this driver's blob gives USED, other bytes give IGNORED, no blob gives
// NOT_SPECIFIED. The cap (Supported TRUE) is checked by engine-ddi/tests/caps-test.cpp, the CORE 0113 slots of the
// table by engine-ddi/tests/modern-core-test.cpp.
#include "app-state-ddi.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#if !defined(D3D12DDI_BUILD_VERSION_0119)
int main() {
    std::puts("SKIPPED: the selected WDK header has no CORE 0113 types");
}
#else
namespace {
namespace as = amdgpu_wddm::app_settings;
namespace st = native12::app_state;

// The runtime's QueryAdapterInfoCb2 for D3DDDI_QUERYADAPTERTYPE_APPLICATIONSPECIFICDRIVERBLOB. With fail set, it
// returns fail. Without a blob, D3DDDIERR_NOTAVAILABLE. With a blob, S_OK and the copy when the buffer is large
// enough, else E_NOT_SUFFICIENT_BUFFER and the required size.
struct Runtime {
    HRESULT fail = S_OK;
    bool has_blob = false;
    std::vector<uint8_t> blob;
    unsigned calls = 0;
    HRESULT operator()(D3DDDICB_QUERYADAPTERINFO2* q) {
        ++calls;
        assert(q && q->QueryType == D3DDDI_QUERYADAPTERTYPE_APPLICATIONSPECIFICDRIVERBLOB);
        if (fail != S_OK) return fail;
        if (!has_blob) return D3DDDIERR_NOTAVAILABLE;
        if (q->PrivateDriverDataSize < blob.size()) {
            q->PrivateDriverDataSize = static_cast<UINT>(blob.size());
            return E_NOT_SUFFICIENT_BUFFER;
        }
        assert(q->pPrivateDriverData);
        if (!blob.empty()) std::memcpy(q->pPrivateDriverData, blob.data(), blob.size());
        q->PrivateDriverDataSize = static_cast<UINT>(blob.size());
        return S_OK;
    }
};

D3D12DDI_HDEVICE handle(native12::Device& d) { return {&d}; }

// Opens the application-specific driver state of a new device against r and returns the device's blob status.
D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_STATUS open(native12::Device& d, Runtime* r, st::Found* found = nullptr) {
    assert(native12::open_app_state(d, r, found));
    assert(d.app_state);
    return native12::app_state_blob_status(handle(d));
}

std::vector<uint8_t> get_blob(native12::Device& d) {
    const UINT size = native12::app_state_blob_size(handle(d));
    assert(size >= st::kFixedBytes && size <= st::kMaxBytes);
    std::vector<uint8_t> out(size + 8, 0xCD);
    assert(native12::app_state_blob(handle(d), out.data(), size + 8) == S_OK);
    for (size_t i = size; i < out.size(); ++i) assert(out[i] == 0xCD);   // nothing written past the size
    out.resize(size);
    return out;
}

// A record that the process settings of this test cannot have: the image name and three set values.
st::Record captured() {
    st::Record r;
    r.settings.values[static_cast<unsigned>(as::Setting::FrameRateLimit)] = {60, as::Source::Application};
    r.settings.values[static_cast<unsigned>(as::Setting::VSync)] = {1, as::Source::Global};
    r.settings.values[static_cast<unsigned>(as::Setting::Anisotropy)] = {16, as::Source::Profile};
    const wchar_t name[] = L"capturedgame.exe";
    std::memcpy(r.settings.application, name, sizeof(name));
    return r;
}
std::vector<uint8_t> encoded(const st::Record& r, uint64_t version) {
    std::vector<uint8_t> out(st::encoded_size(r));
    assert(st::encode(r, version, out.data(), out.size()) == out.size());
    return out;
}

unsigned engine_calls = 0;
D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY engine_check(D3D12DDI_HDEVICE, D3D12DDI_SERIALIZED_DATA_TYPE type,
    const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054*) {
    ++engine_calls;
    assert(type == D3D12DDI_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE);
    return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
}

void test_identifier() {
    const uint64_t version = st::module_file_version();
    const auto own = st::identifier(version);
    assert(st::check(&own, version) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE);
    assert(st::check(nullptr, version) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED);
    auto foreign = own; foreign.DriverOpaqueGUID.Data1 ^= 1;
    assert(st::check(&foreign, version) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED);
    for (unsigned byte = 0; byte < 16; ++byte) {
        auto other = own; other.DriverOpaqueVersioningData[byte] ^= 0x40;
        assert(st::check(&other, version) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION);
    }
    // Another driver version of this driver: the same GUID, other versioning data.
    const auto older = st::identifier(version ^ 0x0001000000000000ull);
    assert(st::check(&older, version) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION);

    // The DDI entry: type 1 is the shell's, every other type goes to the engine.
    native12::Device d;
    native12::engine_check_identifier.store(engine_check);
    assert(native12::check_driver_matching_identifier(handle(d), D3D12DDI_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE,
        &own) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE);
    assert(native12::check_driver_matching_identifier(handle(d), D3D12DDI_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE,
        &older) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION);
    assert(!engine_calls);
    assert(native12::check_driver_matching_identifier(handle(d), D3D12DDI_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
        &own) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED);
    assert(engine_calls == 1);
    native12::engine_check_identifier.store(nullptr);
    assert(native12::check_driver_matching_identifier(handle(d), D3D12DDI_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
        &own) == D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE);
}

void test_format() {
    const uint64_t version = 0x0007000000E40065ull;
    const st::Record r = captured();
    const auto blob = encoded(r, version);
    assert(blob.size() == st::kFixedBytes + 16 * 2);
    // The layout of app-state.h: identifier, payload size, engine part, settings, image name.
    D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 id;
    std::memcpy(&id, blob.data(), sizeof(id));
    assert(!std::memcmp(&id.DriverOpaqueGUID, &st::kStateGuid, sizeof(GUID)));
    uint32_t u32; uint64_t u64;
    std::memcpy(&u32, id.DriverOpaqueVersioningData, 4); assert(u32 == st::kStateFormat);
    std::memcpy(&u32, id.DriverOpaqueVersioningData + 4, 4); assert(u32 == 0);
    std::memcpy(&u64, id.DriverOpaqueVersioningData + 8, 8); assert(u64 == version);
    std::memcpy(&u32, blob.data() + 32, 4); assert(u32 == blob.size() - 36);
    std::memcpy(&u32, blob.data() + 36, 4); assert(u32 == 0);                  // no engine part in this build
    std::memcpy(&u64, blob.data() + 40, 8); assert(u64 == 0);
    std::memcpy(&u32, blob.data() + 48, 4); assert(u32 == as::kCount);
    std::memcpy(&u32, blob.data() + 52, 4); assert(u32 == 60);                 // FrameRateLimit value
    std::memcpy(&u32, blob.data() + 56, 4); assert(u32 == static_cast<uint32_t>(as::Source::Application));
    std::memcpy(&u32, blob.data() + 52 + as::kCount * 8, 4); assert(u32 == 16);  // name units
    assert(st::encode(r, version, std::vector<uint8_t>(blob.size() - 1).data(), blob.size() - 1) == 0);

    st::Record back;
    assert(st::decode(blob.data(), blob.size(), version, back));
    assert(!std::memcmp(back.settings.values, r.settings.values, sizeof(r.settings.values)));
    assert(!std::wcscmp(back.settings.application, L"capturedgame.exe"));
    assert(!back.engine.valid && !back.engine.config_flags);
    assert(encoded(back, version) == blob);

    // An empty image name and no set value: the smallest blob.
    const auto empty = encoded(st::Record{}, version);
    assert(empty.size() == st::kFixedBytes && st::decode(empty.data(), empty.size(), version, back));

    // Every change that makes the blob one this driver does not write: refused, out unchanged.
    const auto refused = [&](std::vector<uint8_t> b) {
        st::Record out = captured();
        out.settings.values[0].value = 77;
        const bool ok = st::decode(b.data(), b.size(), version, out);
        return !ok && out.settings.values[0].value == 77;
    };
    const auto patch32 = [&](size_t at, uint32_t v) { auto b = blob; std::memcpy(b.data() + at, &v, 4); return b; };
    assert(!st::decode(blob.data(), blob.size(), version + 1, back));          // another driver version
    assert(refused(std::vector<uint8_t>(blob.begin(), blob.end() - 2)));      // truncated
    { auto b = blob; b.push_back(0); b.push_back(0); assert(refused(b)); }    // trailing bytes
    assert(refused(patch32(32, static_cast<uint32_t>(blob.size() - 36 + 2))));  // payload size
    assert(refused(patch32(36, 1)));                                          // engine part this build cannot apply
    assert(refused(patch32(36, 2)));
    { auto b = blob; const uint64_t f = 1; std::memcpy(b.data() + 40, &f, 8); assert(refused(b)); }  // flags, not valid
    assert(refused(patch32(48, as::kCount + 1)));                             // setting count
    assert(refused(patch32(52, 5)));                                          // FrameRateLimit 5 is out of range
    assert(refused(patch32(56, static_cast<uint32_t>(as::Source::Profile) + 1)));  // source
    assert(refused(patch32(52 + 8 * 3 + 0, 3)));                              // MaxFrameLatency 3, source None
    assert(refused(patch32(52 + as::kCount * 8, 15)));                        // name units
    {
        auto b = blob; const uint16_t slash = L'\\';
        std::memcpy(b.data() + st::kFixedBytes + 4, &slash, 2); assert(refused(b));   // a path, not a file name
        const uint16_t nul = 0;
        std::memcpy(b.data() + st::kFixedBytes + 4, &nul, 2); assert(refused(b));
    }
    assert(!st::decode(nullptr, blob.size(), version, back));
}

void test_device() {
    const uint64_t version = st::module_file_version();
    const as::Settings& process = as::process_settings();
    {   // No QueryAdapterInfoCb2: NOT_SPECIFIED, the process settings.
        native12::Device d;
        assert(open(d, static_cast<Runtime*>(nullptr)) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED);
        assert(&d.settings() == &process);
    }
    {   // No blob set for the adapter: NOT_SPECIFIED.
        native12::Device d; Runtime r; st::Found f;
        assert(open(d, &r, &f) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED);
        assert(r.calls == 1 && f.query == D3DDDIERR_NOTAVAILABLE && &d.settings() == &process);
    }
    {   // A runtime without the query: E_INVALIDARG, NOT_SPECIFIED.
        native12::Device d; Runtime r; r.fail = E_INVALIDARG;
        assert(open(d, &r) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED && &d.settings() == &process);
    }
    {   // Any other failure: the driver cannot tell whether a blob was set.
        native12::Device d; Runtime r; r.fail = E_FAIL;
        assert(open(d, &r) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_UNKNOWN && &d.settings() == &process);
    }

    // Get on a device with no blob gives the device's own profile, which CheckDriverMatchingIdentifier accepts.
    std::vector<uint8_t> own;
    {
        native12::Device d; Runtime r;
        open(d, &r);
        own = get_blob(d);
        D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 id;
        std::memcpy(&id, own.data(), sizeof(id));
        assert(native12::check_driver_matching_identifier(handle(d),
            D3D12DDI_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE, &id) ==
            D3D12DDI_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE);
        st::Record back;
        assert(st::decode(own.data(), own.size(), version, back));
        assert(!std::memcmp(back.settings.values, process.values, sizeof(process.values)));
        // Refusals of Get: no buffer, a buffer one byte short. No device: no size.
        std::vector<uint8_t> short_buffer(own.size() - 1);
        assert(native12::app_state_blob(handle(d), nullptr, static_cast<UINT>(own.size())) == E_INVALIDARG);
        assert(native12::app_state_blob(handle(d), short_buffer.data(), static_cast<UINT>(short_buffer.size())) == E_INVALIDARG);
        assert(native12::app_state_blob_size({}) == 0 && native12::app_state_blob({}, short_buffer.data(), 1) == E_INVALIDARG);
        assert(native12::app_state_blob_status({}) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_UNKNOWN);
    }
    {   // The blob of a device given back: USED.
        native12::Device d; Runtime r; r.has_blob = true; r.blob = own; st::Found f;
        assert(open(d, &r, &f) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_USED);
        assert(f.query == S_OK && f.size == own.size() && r.calls == 1);
        assert(get_blob(d) == own);
    }
    {   // A captured application's blob: USED, the device applies its settings and gives the same blob again.
        native12::Device d; Runtime r; r.has_blob = true; r.blob = encoded(captured(), version);
        assert(open(d, &r) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_USED);
        assert(&d.settings() != &process);
        assert(as::frame_rate_limit(d.settings()) == 60 && as::sync_override(d.settings()).valid &&
               as::sync_override(d.settings()).interval == 1 && as::vkd3d_anisotropy(d.settings()) == "16");
        assert(!std::wcscmp(d.settings().application, L"capturedgame.exe"));
        assert(get_blob(d) == r.blob);
    }
    {   // The blob of another driver version: IGNORED, the process settings.
        native12::Device d; Runtime r; r.has_blob = true; r.blob = encoded(captured(), version ^ 1);
        assert(open(d, &r) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED && &d.settings() == &process);
        assert(get_blob(d) == own);
    }
    {   // Random bytes: IGNORED.
        native12::Device d; Runtime r; r.has_blob = true; r.blob.resize(300);
        uint32_t x = 0x2545F491u;
        for (auto& b : r.blob) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = static_cast<uint8_t>(x); }
        assert(open(d, &r) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED && &d.settings() == &process);
    }
    {   // A blob of zero bytes: IGNORED.
        native12::Device d; Runtime r; r.has_blob = true;
        assert(open(d, &r) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED);
    }
    {   // Larger than the first buffer: a second call with the size the runtime named, then IGNORED (not ours).
        native12::Device d; Runtime r; r.has_blob = true; r.blob = own; r.blob.resize(4000, 0x5A); st::Found f;
        assert(open(d, &r, &f) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED);
        assert(r.calls == 2 && f.query == S_OK && f.size == 4000);
    }
    {   // Above the read limit: IGNORED without a copy.
        native12::Device d; Runtime r; r.has_blob = true; r.blob.resize(st::kReadLimit + 1); st::Found f;
        assert(open(d, &r, &f) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_IGNORED);
        assert(r.calls == 1 && f.query == E_NOT_SUFFICIENT_BUFFER && f.size == st::kReadLimit + 1);
    }
    {   // A device of a build before 0113 has no state: the status entry answers UNKNOWN, Get gives the process profile.
        native12::Device d;
        assert(native12::app_state_blob_status(handle(d)) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_UNKNOWN);
        assert(get_blob(d) == own);
    }
}
} // namespace

int main() {
    test_identifier();
    test_format();
    test_device();
    std::puts("PASSED: application-specific driver state: identifier (COMPATIBLE, INCOMPATIBLE_VERSION, UNRECOGNIZED, "
              "engine types forwarded), blob layout and its refusals, Get, own blob USED, captured blob USED and applied, "
              "other version IGNORED, random bytes IGNORED, no blob NOT_SPECIFIED, query failure UNKNOWN; no runtime/GPU");
}
#endif
