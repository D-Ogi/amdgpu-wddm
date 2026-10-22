// Bounded, named read-only observation. No raw register or write escape.
// HardwareAccess idles GPU scheduling. These sequential reads are not atomic.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <dxgi1_2.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../../../driver/kmd/bc250kmd_escape.h"

static_assert(sizeof(BC250_ESCAPE_DCN_OBSERVE) == 128, "observer ABI");
static_assert(sizeof(unsigned long) == 4, "Windows DWORD");
static const char *fields[] = {
    "Magic", "Command", "Status", "Version", "NtStatus", "AbiVersion", "RegisterCount", "ValidMask",
    "PrimaryAddressLow", "PrimaryAddressHigh", "EarliestInUseLow", "EarliestInUseHigh",
    "FlipControl", "SurfacePitch", "OtgStatusPosition", "OtgGlobalControl0", "OtgBlankControl",
    "OtgDoubleBufferControl", "OtgFrameCount", "TimingControl", "TimingHTotal", "TimingVTotal",
    "TimingHBlank", "TimingVBlank", "TimingPixelControl", "TimingPhase", "TimingModulo",
    "TimingInterlace", "TimingVTotalControl", "TimingReference", "SequenceBefore", "SequenceAfter"
};
static_assert(sizeof(fields) / sizeof(fields[0]) == sizeof(BC250_ESCAPE_DCN_OBSERVE) / 4, "field count");

static bool number(const char *text, unsigned long *result)
{
    char *end = nullptr;
    if (text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    unsigned long value = strtoul(text, &end, 0);
    if (errno || *end) return false;
    *result = value;
    return true;
}

static int error(const char *stage, unsigned long code)
{
    fprintf(stderr, "{\"type\":\"error\",\"stage\":\"%s\",\"code\":%lu}\n", stage, code);
    return 1;
}

// A DXGI LUID identifies the exact selected adapter. Refuse zero or multiple BC-250 devices.
static bool select_adapter(LUID *luid)
{
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
    if (FAILED(hr)) { error("factory", (unsigned long)hr); return false; }
    unsigned found = 0;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { factory->Release(); error("enumeration", (unsigned long)hr); return false; }
        DXGI_ADAPTER_DESC1 desc = {};
        hr = adapter->GetDesc1(&desc);
        adapter->Release();
        if (FAILED(hr)) { factory->Release(); error("description", (unsigned long)hr); return false; }
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == 0x1002 && desc.DeviceId == 0x13fe) {
            *luid = desc.AdapterLuid;
            ++found;
        }
    }
    factory->Release();
    if (found != 1) { error("unique-bc250", found); return false; }
    return true;
}

static NTSTATUS read_named(D3DKMT_HANDLE adapter, BC250_ESCAPE_DCN_OBSERVE *data)
{
    *data = {};
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_OBSERVE_DCN;
    data->AbiVersion = BC250_DCN_OBSERVE_ABI;
    D3DKMT_ESCAPE request = {};
    request.hAdapter = adapter;
    request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    request.Flags.Value = 1; // HardwareAccess only: preserve the KMD's MMIO lifetime exclusion.
    request.pPrivateDriverData = data;
    request.PrivateDriverDataSize = sizeof(*data);
    return D3DKMTEscape(&request);
}

int main(int argc, char **argv)
{
    unsigned long version = 0, samples = 20, interval = 250;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("dcn_observe --expected-version <KMD version word> [--samples 1..40] [--interval-ms 100..500]");
        puts("Administrator required. Read-only but idles GPU scheduling. Run with a 30-second external watchdog.");
        return 0;
    }
    if (argc < 3 || !(argc & 1)) return error("arguments", 0);
    unsigned seen = 0;
    for (int i = 1; i < argc; i += 2) {
        unsigned flag = !strcmp(argv[i], "--expected-version") ? 1u :
                        !strcmp(argv[i], "--samples") ? 2u : !strcmp(argv[i], "--interval-ms") ? 4u : 0u;
        unsigned long *target = flag == 1 ? &version : flag == 2 ? &samples : &interval;
        if (!flag || (seen & flag) || !number(argv[i + 1], target)) return error("arguments", 0);
        seen |= flag;
    }
    if (!(seen & 1) || !version || samples < 1 || samples > 40 || interval < 100 || interval > 500)
        return error("bounds", 0);
    LARGE_INTEGER frequency = {}, start = {}, before = {}, after = {};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return error("qpc", 0);
    LUID luid = {};
    if (!select_adapter(&luid)) return 1;
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = luid;
    NTSTATUS status = D3DKMTOpenAdapterFromLuid(&open);
    if (status < 0) return error("open", (unsigned long)status);
    D3DKMT_CLOSEADAPTER close = {};
    close.hAdapter = open.hAdapter;
    BC250_ESCAPE info = {};
    info.Magic = BC250_ESCAPE_MAGIC;
    info.Command = BC250_ESCAPE_GET_INFO;
    D3DKMT_ESCAPE infoRequest = {};
    infoRequest.hAdapter = open.hAdapter;
    infoRequest.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    infoRequest.pPrivateDriverData = &info;
    infoRequest.PrivateDriverDataSize = sizeof(info);
    status = D3DKMTEscape(&infoRequest);
    if (status != 0 || info.Magic != BC250_ESCAPE_MAGIC || info.Command != BC250_ESCAPE_GET_INFO ||
        info.Status != BC250_ESCAPE_STATUS_DONE || info.Version != version) {
        D3DKMTCloseAdapter(&close);
        return error("identity-before-register-reads", (unsigned long)status);
    }
    printf("{\"type\":\"header\",\"schema\":1,\"expected_version\":%lu,\"samples\":%lu,"
           "\"interval_ms\":%lu,\"qpc_frequency\":%lld,\"hardware_access\":true,\"atomic\":false}\n",
           version, samples, interval, frequency.QuadPart);
    fflush(stdout);
    QueryPerformanceCounter(&start);
    int result = 0;
    for (unsigned long index = 0; index < samples; ++index) {
        QueryPerformanceCounter(&before);
        if ((before.QuadPart - start.QuadPart) / frequency.QuadPart >= 25) { result = error("deadline", 25); break; }
        BC250_ESCAPE_DCN_OBSERVE data = {};
        status = read_named(open.hAdapter, &data);
        QueryPerformanceCounter(&after);
        unsigned long words[32];
        memcpy(words, &data, sizeof(words));
        printf("{\"type\":\"sample\",\"index\":%lu,\"begin_qpc\":%lld,\"end_qpc\":%lld,"
               "\"escape_status\":%lu,\"data\":{", index, before.QuadPart, after.QuadPart, (unsigned long)status);
        for (unsigned i = 0; i < 32; ++i) printf("%s\"%s\":%lu", i ? "," : "", fields[i], words[i]);
        puts("}}");
        fflush(stdout);
        if (status < 0 || data.Magic != BC250_ESCAPE_MAGIC || data.Command != BC250_ESCAPE_OBSERVE_DCN ||
            data.Version != version || data.AbiVersion != BC250_DCN_OBSERVE_ABI ||
            data.Status != BC250_ESCAPE_STATUS_DONE || data.NtStatus != 0 ||
            data.RegisterCount != BC250_DCN_OBSERVE_REG_COUNT || data.ValidMask != BC250_DCN_OBSERVE_VALID_ALL) {
            result = error("refused-or-mismatched-reply", data.NtStatus); break;
        }
        if (index + 1 < samples) Sleep(interval);
    }
    status = D3DKMTCloseAdapter(&close);
    if (status < 0) result = error("close", (unsigned long)status);
    printf("{\"type\":\"end\",\"exit_code\":%d}\n", result);
    return result;
}
