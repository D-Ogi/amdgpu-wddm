// SPDX-License-Identifier: MIT
// BD-296 part (b): the compositor report of the router in dwm.exe (BC250_ESCAPE_SET_DESKTOP_COMPOSITOR,
// driver/kmd/bc250kmd_escape.h). At each desktop decision the router tells the kernel driver which UMD composes:
// with a CPU route the KMD gives the standard GDI textures created after the report a cached CPU view in the aperture
// (driver/kmd/gdi_admission.h, Bc250GdiCompositorPlacement), so that the CPU UMD can map the window contents that
// GDI draws. Without the report a CPU compositor of a start whose KMD decided the GPU route (a hosted open that
// failed, a kill switch that only the router reads) has no window contents. The UMD safety net (part (a)) keeps the
// device alive in that case; this report gives the contents back.
//
// The adapter: the LUID of the identity trailer of the adapter query (bc250_adapter_identity.h), which the runtime's
// pfnQueryAdapterInfoCb gives before any device exists. The escape goes through D3DKMTOpenAdapterFromLuid, so it
// needs no device, context or allocation. The KMD accepts it only from a Window Manager account (the token user of
// dwm.exe). A failure is reported in the route line ("kmd=" column) and never changes the route or its HRESULT: the
// placement then stays as it was, which is the state before this report existed.
//
// The functions are injected (CompositorKmt), so that the router's tests run every answer with no kernel driver.
#pragma once
#include <windows.h>
#include <d3dkmthk.h>
#include <d3d10umddi.h>
#include <cstdio>
#include <cstring>
#include "../../contract/bc250_adapter_identity.h"
#include "../../contract/bc250_scanout_caps.h"
#include "../../contract/bc250_desktop_route.h"
#include "../../kmd/bc250kmd_escape.h"

namespace bc250router {

// The escape takes the words of the session record unchanged: one route word for both readers.
static_assert(BC250_COMPOSITOR_ROUTE_GPU == BC250_DESKTOP_ROUTE_GPU, "route word GPU");
static_assert(BC250_COMPOSITOR_ROUTE_KILL_SWITCH == BC250_DESKTOP_ROUTE_KILL_SWITCH, "route word kill switch");
static_assert(BC250_COMPOSITOR_ROUTE_SWITCHES_OFF == BC250_DESKTOP_ROUTE_SWITCHES_OFF, "route word switches off");
static_assert(BC250_COMPOSITOR_ROUTE_FALLBACK == BC250_DESKTOP_ROUTE_FALLBACK, "route word fallback");
static_assert(sizeof(BC250_ESCAPE_DESKTOP_COMPOSITOR) == 80, "escape ABI 1 is 80 bytes");

struct CompositorKmt {
    PFND3DKMT_OPENADAPTERFROMLUID open = nullptr;
    PFND3DKMT_ESCAPE escape = nullptr;
    PFND3DKMT_CLOSEADAPTER close = nullptr;
};

// The request of one report. Route: a BC250_DESKTOP_ROUTE_* word other than NONE.
inline BC250_ESCAPE_DESKTOP_COMPOSITOR CompositorRequest(unsigned route, unsigned long pid)
{
    BC250_ESCAPE_DESKTOP_COMPOSITOR r = {};
    r.Magic = BC250_ESCAPE_MAGIC;
    r.Command = BC250_ESCAPE_SET_DESKTOP_COMPOSITOR;
    r.AbiVersion = BC250_DESKTOP_COMPOSITOR_ABI;
    r.Route = route;
    r.Pid = pid;
    return r;
}

// The adapter LUID from the identity trailer. The buffer covers every trailer the kernel driver may write
// (driver/umd/dxvk/adapter-identity.h has the reason). False on a failed query or a trailer of another shape.
inline bool CompositorAdapterLuid(HANDLE adapter, PFND3DDDI_QUERYADAPTERINFOCB query, LUID *luid, HRESULT *hr)
{
    *hr = E_INVALIDARG;
    if (!adapter || !query || !luid) return false;
    unsigned char data[BC250_SCANOUT_CAPS_TOTAL] = {};
    D3DDDICB_QUERYADAPTERINFO request = {};
    request.pPrivateDriverData = data;
    request.PrivateDriverDataSize = sizeof(data);
    *hr = query(adapter, &request);
    if (FAILED(*hr)) return false;
    bc250_adapter_identity identity = {};
    memcpy(&identity, data + BC250_ADAPTER_IDENTITY_OFFSET, sizeof(identity));
    if (identity.magic != BC250_ADAPTER_IDENTITY_MAGIC || identity.version != BC250_ADAPTER_IDENTITY_VERSION ||
        identity.size != sizeof(identity) || identity.reserved || (!identity.luid_low && !identity.luid_high)) {
        *hr = DXGI_ERROR_UNSUPPORTED;
        return false;
    }
    luid->LowPart = identity.luid_low;
    luid->HighPart = (LONG)identity.luid_high;
    *hr = S_OK;
    return true;
}

// One report. The answer is the word of the route line's kmd= column:
//   cpu-<placed> / gpu-<placed>   accepted; the KMD now places for the CPU or the GPU compositor; <placed> is the
//                                  count of standard textures placed with a CPU view in this start
//   cpu-noaperture-<placed>        accepted with a CPU route, but this start offers no aperture: no texture moves
//   refused-<reason>               the KMD refused it (BC250_COMPOSITOR_REASON_*: 1 ABI, 2 route, 3 account)
//   identity-<hr>                  the adapter query gave no identity trailer
//   open-<nt> / escape-<nt>        D3DKMTOpenAdapterFromLuid or D3DKMTEscape failed (an older KMD answers an
//                                  unknown command with an escape failure)
//   nokmt                          gdi32 has no D3DKMT entry point
inline const char *ReportCompositor(HANDLE adapter, PFND3DDDI_QUERYADAPTERINFOCB query, const CompositorKmt &kmt,
                                    unsigned route, unsigned long pid, char *word, size_t chars)
{
    if (!kmt.open || !kmt.escape || !kmt.close) return "nokmt";
    LUID luid = {};
    HRESULT hr = S_OK;
    if (!CompositorAdapterLuid(adapter, query, &luid, &hr)) {
        _snprintf_s(word, chars, _TRUNCATE, "identity-%08lx", (unsigned long)hr);
        return word;
    }
    D3DKMT_OPENADAPTERFROMLUID opened = {};
    opened.AdapterLuid = luid;
    const NTSTATUS open = kmt.open(&opened);
    if (open != 0 || !opened.hAdapter) {
        _snprintf_s(word, chars, _TRUNCATE, "open-%08lx", (unsigned long)open);
        return word;
    }
    BC250_ESCAPE_DESKTOP_COMPOSITOR data = CompositorRequest(route, pid);
    D3DKMT_ESCAPE e = {};
    e.hAdapter = opened.hAdapter;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = &data;
    e.PrivateDriverDataSize = sizeof(data);
    const NTSTATUS escaped = kmt.escape(&e);
    D3DKMT_CLOSEADAPTER closing = {};
    closing.hAdapter = opened.hAdapter;
    kmt.close(&closing);
    if (escaped != 0) _snprintf_s(word, chars, _TRUNCATE, "escape-%08lx", (unsigned long)escaped);
    else if (data.Status != BC250_ESCAPE_STATUS_DONE)
        _snprintf_s(word, chars, _TRUNCATE, "refused-%lu", data.Reason);
    else if (data.CpuCompositor && !data.ApertureOffered)
        _snprintf_s(word, chars, _TRUNCATE, "cpu-noaperture-%lu", data.Placed);
    else
        _snprintf_s(word, chars, _TRUNCATE, "%s-%lu", data.CpuCompositor ? "cpu" : "gpu", data.Placed);
    return word;
}

// The D3DKMT entry points of gdi32 in System32 (never the application's directory). Resolved once per process.
inline CompositorKmt SystemCompositorKmt()
{
    static CompositorKmt kmt;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(
        &once,
        [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
            HMODULE gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (gdi) {
                kmt.open = (PFND3DKMT_OPENADAPTERFROMLUID)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
                kmt.escape = (PFND3DKMT_ESCAPE)GetProcAddress(gdi, "D3DKMTEscape");
                kmt.close = (PFND3DKMT_CLOSEADAPTER)GetProcAddress(gdi, "D3DKMTCloseAdapter");
            }
            return TRUE;
        },
        nullptr, nullptr);
    return kmt;
}

} // namespace bc250router
