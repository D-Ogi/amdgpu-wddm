// SPDX-License-Identifier: MIT
// Host-test double of a UMD for the router tests. Built five times:
//   FAKE_TAG=0xC0 (CPU), FAKE_TAG=0x60 (hosted), FAKE_TAG=0xA0 (application GPU UMD), FAKE_TAG=0xFA with FAKE_FAIL
//   (a GPU UMD whose OpenAdapter scribbles over its outputs and then fails, as a broken GPU path could), and
//   FAKE_TAG=0xB0 with FAKE_NO_OA102 (a GPU UMD without the OpenAdapter10_2 export).
// On success it tags hAdapter.pDrvPrivate with FAKE_TAG + the entry number and fills every table slot.
// The doubles refuse (E_UNEXPECTED) arguments that are not the harness's clean ones, which proves that the
// router restored them after a failed GPU call.
//
// M15.14: the double is also a D3D10.0 DEVICE double, so the host gate can drive the router's D3D11_1 front
// (driver/umd/router/front-adapter.h) end to end without a GPU. It fills all five adapter entries, every one of
// the 101 D3D10DDI_DEVICEFUNCS slots and all 7 DXGI_DDI_BASE_FUNCTIONS slots, and it RECORDS the entries the
// front forwarded to it, in call order, for FakeRecord. Slots the gate never calls are filled with distinct
// recognizable values rather than with functions: FakeDeviceFuncs hands the gate the same table, so a slot of
// the front's published table is either one of these values (a field copy) or not (the front's own body), and
// the gate can tell the two apart by pointer identity alone.
#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#pragma warning(pop)
#include <cstdio>
#include <cstring>

#ifndef FAKE_TAG
#error FAKE_TAG
#endif

// ---------------------------------------------------------------- the record

// One line per forwarded entry, in call order. A fixed buffer: the gate drives a handful of entries and a
// full buffer would hide a later line, so the last line is a marker instead of a silent truncation.
static char Record[4096];
static size_t RecordUsed;

static void Note(const char *what)
{
    const size_t n = strlen(what);
    if (RecordUsed + n + 2 >= sizeof(Record)) {
        strcpy_s(Record + sizeof(Record) - 10, 10, "overflow\n");
        return;
    }
    memcpy(Record + RecordUsed, what, n);
    RecordUsed += n;
    Record[RecordUsed++] = '\n';
    Record[RecordUsed] = 0;
}

extern "C" __declspec(dllexport) const char *FakeRecord(void) { return Record; }
extern "C" __declspec(dllexport) void FakeRecordReset(void) { RecordUsed = 0; Record[0] = 0; }

// ---------------------------------------------------------------- the device double

// The private device size the double asks for, and the byte it writes over all of it. The front places its own
// record after this block, so the gate can check that the pattern survived the front's CreateDevice.
static const SIZE_T FakeDeviceBytes = 512;
static const int FakeDevicePattern = 0x5D;
// The value in device-table slot i. Distinct, non-null, never called, and far from any code address.
static const UINT_PTR FakeSlotBase = 0x0510000;
extern "C" __declspec(dllexport) UINT_PTR FakeDeviceSlot(unsigned i) { return FakeSlotBase + i; }
extern "C" __declspec(dllexport) SIZE_T FakeDeviceSize(void) { return FakeDeviceBytes; }

static VOID APIENTRY FakeDestroyDevice(D3D10DDI_HDEVICE) { Note("destroy-device"); }
static VOID APIENTRY FakeFlush(D3D10DDI_HDEVICE) { Note("flush"); }
static VOID APIENTRY FakeRelocateDeviceFuncs(D3D10DDI_HDEVICE, D3D10DDI_DEVICEFUNCS *) { Note("relocate-device-funcs"); }
static VOID APIENTRY FakeClearRenderTargetView(D3D10DDI_HDEVICE, D3D10DDI_HRENDERTARGETVIEW view, FLOAT color[4])
{
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "clear-rtv view=%p color=%.1f,%.1f,%.1f,%.1f", view.pDrvPrivate,
                color[0], color[1], color[2], color[3]);
    Note(line);
}
static VOID APIENTRY FakeClearDepthStencilView(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILVIEW view, UINT flags,
                                               FLOAT depth, UINT8 stencil)
{
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "clear-dsv view=%p flags=%u depth=%.1f stencil=%u",
                view.pDrvPrivate, flags, depth, (unsigned)stencil);
    Note(line);
}

// Every slot, in one place. Called by the double's CreateDevice and exported for the gate, so the two can
// never disagree about what the hosted table holds.
extern "C" __declspec(dllexport) void FakeDeviceFuncs(D3D10DDI_DEVICEFUNCS *out)
{
    if (!out) return;
    void **slots = (void **)out;
    for (unsigned i = 0; i < sizeof(*out) / sizeof(void *); ++i) slots[i] = (void *)FakeDeviceSlot(i);
    out->pfnDestroyDevice = FakeDestroyDevice;
    out->pfnFlush = FakeFlush;
    out->pfnRelocateDeviceFuncs = FakeRelocateDeviceFuncs;
    out->pfnClearRenderTargetView = FakeClearRenderTargetView;
    out->pfnClearDepthStencilView = FakeClearDepthStencilView;
}

static SIZE_T APIENTRY Size(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *args)
{
    // The front rewrites Interface to the D3D10.0 pair before it asks. A double that answered for any
    // interface would hide a front that forgot to.
    if (!args || args->Interface != D3D10_0_DDI_INTERFACE_VERSION) { Note("calc-size-wrong-interface"); return 0; }
    Note("calc-private-device-size");
    return FakeDeviceBytes;
}

// The failing double never gets as far as a device: its OpenAdapter refuses. Its four adapter entries would
// then be functions with internal linkage that nothing references (C4505, and /WX makes that an error).
#ifndef FAKE_FAIL
static HRESULT APIENTRY CreateDeviceFn(D3D10DDI_HADAPTER, D3D10DDIARG_CREATEDEVICE *args)
{
    if (!args || !args->pDeviceFuncs) { Note("create-device-no-table"); return E_INVALIDARG; }
    if (args->Interface != D3D10_0_DDI_INTERFACE_VERSION) { Note("create-device-wrong-interface"); return E_INVALIDARG; }
    if (!args->hDrvDevice.pDrvPrivate) { Note("create-device-no-storage"); return E_INVALIDARG; }
    memset(args->hDrvDevice.pDrvPrivate, FakeDevicePattern, FakeDeviceBytes);
    FakeDeviceFuncs(args->pDeviceFuncs);
    // A D3D10.0 driver writes the 7 entries it has through the union member, whatever bigger struct the
    // runtime put behind the pointer. The front then reads those 7 back and fills the rest.
    if (args->DXGIBaseDDI.pDXGIDDIBaseFunctions) {
        void **dxgi = (void **)args->DXGIBaseDDI.pDXGIDDIBaseFunctions;
        for (unsigned i = 0; i < sizeof(DXGI_DDI_BASE_FUNCTIONS) / sizeof(void *); ++i)
            dxgi[i] = (void *)(FakeSlotBase + 0x1000 + i);
    }
    Note("create-device");
    // The Mesa frontend answers DXGI_STATUS_NO_REDIRECTION and not S_OK (Device.cpp). The double answers the
    // same, so the gate fails a front that flattens a hosted success code into S_OK.
    return DXGI_STATUS_NO_REDIRECTION;
}

static HRESULT APIENTRY CloseAdapterFn(D3D10DDI_HADAPTER) { Note("close-adapter"); return S_OK; }

// Two versions, so that the front's GetSupportedVersions has something to append to and the count-query and
// short-array protocol can be checked.
static const UINT64 FakeVersions[] = {D3D10_0_DDI_SUPPORTED, D3D10_0_x_DDI_SUPPORTED};

static HRESULT APIENTRY GetSupportedVersionsFn(D3D10DDI_HADAPTER, UINT32 *entries, UINT64 *versions)
{
    if (!entries) return E_INVALIDARG;
    if (!versions) {
        Note("get-supported-versions-count");
        *entries = (UINT32)ARRAYSIZE(FakeVersions);
        return S_OK;
    }
    if (*entries < ARRAYSIZE(FakeVersions)) { Note("get-supported-versions-short"); return E_OUTOFMEMORY; }
    Note("get-supported-versions-fill");
    memcpy(versions, FakeVersions, sizeof(FakeVersions));
    *entries = (UINT32)ARRAYSIZE(FakeVersions);
    return S_OK;
}

// The hosted driver's GetCaps zeroes the buffer and returns S_OK for every type, as the Mesa frontend does.
// The front must not forward the D3D11-era types to it, and the gate checks exactly that.
static HRESULT APIENTRY GetCapsFn(D3D10DDI_HADAPTER, const D3D10_2DDIARG_GETCAPS *args)
{
    char line[64];
    if (!args) return E_INVALIDARG;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "get-caps type=%d", (int)args->Type);
    Note(line);
    if (args->pData && args->DataSize) memset(args->pData, 0, args->DataSize);
    return S_OK;
}
#endif

static HRESULT Fill(D3D10DDIARG_OPENADAPTER *a, bool v2, UINT_PTR entry)
{
    if (!a || !a->pAdapterFuncs) return E_INVALIDARG;
#ifdef FAKE_FAIL
    (void)entry;
    a->hAdapter.pDrvPrivate = (void *)(UINT_PTR)0xBADBAD;
    a->Interface = 0xBADBAD;
    a->pAdapterFuncs->pfnCalcPrivateDeviceSize = Size;
    if (v2) a->pAdapterFuncs_2->pfnGetCaps = (PFND3D10_2DDI_GETCAPS)(UINT_PTR)0xBADBAD;
    return E_FAIL;
#else
    // The harness passes hAdapter NULL, every table slot NULL and hRTAdapter 0x5A5A.
    if (a->hAdapter.pDrvPrivate || a->hRTAdapter.handle != (HANDLE)(UINT_PTR)0x5A5A ||
        a->pAdapterFuncs->pfnCalcPrivateDeviceSize || (v2 && a->pAdapterFuncs_2->pfnGetCaps))
        return E_UNEXPECTED;
    a->hAdapter.pDrvPrivate = (void *)(UINT_PTR)(FAKE_TAG * 1000 + entry);
    a->pAdapterFuncs->pfnCalcPrivateDeviceSize = Size;
    a->pAdapterFuncs->pfnCreateDevice = CreateDeviceFn;
    a->pAdapterFuncs->pfnCloseAdapter = CloseAdapterFn;
    // The front needs all five, and it refuses an incomplete hosted table. The D3D10.0 entry has no room for
    // the last two, so OpenAdapter10 leaves them where the runtime put them.
    if (v2) {
        a->pAdapterFuncs_2->pfnGetSupportedVersions = GetSupportedVersionsFn;
        a->pAdapterFuncs_2->pfnGetCaps = GetCapsFn;
    }
    return S_OK;
#endif
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *a) { return Fill(a, false, 10); }
#ifndef FAKE_NO_OA102
extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *a) { return Fill(a, true, 102); }
#endif
