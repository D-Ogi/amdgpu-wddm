// SPDX-License-Identifier: MIT
// Host-test double of a UMD for the router tests. Built five times:
//   FAKE_TAG=0xC0 (CPU), FAKE_TAG=0x60 (hosted), FAKE_TAG=0xA0 (application GPU UMD), FAKE_TAG=0xFA with FAKE_FAIL
//   (a GPU UMD whose OpenAdapter scribbles over its outputs and then fails, as a broken GPU path could), and
//   FAKE_TAG=0xB0 with FAKE_NO_OA102 (a GPU UMD without the OpenAdapter10_2 export).
// On success it tags hAdapter.pDrvPrivate with FAKE_TAG + the entry number and fills every table slot.
// The doubles refuse (E_UNEXPECTED) arguments that are not the harness's clean ones, which proves that the
// router restored them after a failed GPU call.
#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#pragma warning(pop)

#ifndef FAKE_TAG
#error FAKE_TAG
#endif

static SIZE_T APIENTRY Size(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *) { return FAKE_TAG; }

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
    return S_OK;
#endif
}

#ifdef _WIN64
#define UMD_EXPORT __declspec(dllexport)
#else
// x86: __stdcall decorates an exported name (_OpenAdapter10@4), and the D3D runtime asks GetProcAddress for the
// plain one. The linker exports the plain names instead (BD-064).
#define UMD_EXPORT
#pragma comment(linker, "/EXPORT:OpenAdapter10=_OpenAdapter10@4")
#ifndef FAKE_NO_OA102
#pragma comment(linker, "/EXPORT:OpenAdapter10_2=_OpenAdapter10_2@4")
#endif
#endif
extern "C" UMD_EXPORT HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *a) { return Fill(a, false, 10); }
#ifndef FAKE_NO_OA102
extern "C" UMD_EXPORT HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *a) { return Fill(a, true, 102); }
#endif
