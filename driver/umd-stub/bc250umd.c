// M7 stage A: a user-mode display driver that opens nothing.
//
// A full WDDM miniport is only half a graphics stack. dxgkrnl starts the adapter, but the moment anything wants to
// render, the Direct3D runtime loads the DLL the adapter's UserModeDriverName names and calls its OpenAdapter
// entry point. Stage A's question (docs/research/m7-full-wddm-miniport.md section 2.3) is what the runtime and the
// desktop do when that step fails, and there are two ways for it to fail:
//
//   no DLL at all     - UserModeDriverName is absent, so the runtime cannot even load a driver;
//   this DLL          - the DLL loads, exports what the runtime looks for, and every entry point says E_NOTIMPL.
//
// The two are not the same failure and may not produce the same behaviour in dwm.exe. Stage A is therefore run
// once without the UserModeDriverName lines in the INF and once with them (driver/kmd/README.md), and this is the
// DLL for the second run. It does nothing else: no device, no memory, no escape, no thread, no file, no registry.
//
// The three entry points, in the order the REG_MULTI_SZ lists them:
//
//   OpenAdapter       D3D9,  d3d9.dll        PFND3DDDI_OPENADAPTER    (d3dumddi.h)
//   OpenAdapter10     D3D10, d3d10core.dll   PFND3D10DDI_OPENADAPTER  (d3d10umddi.h)
//   OpenAdapter10_2   D3D11, d3d11.dll       PFND3D10DDI_OPENADAPTER  (d3d10umddi.h, the 10.1/11 function table)
//
// Which one is called first is a stage A measurement, not an assumption: D3D11 is what DWM composes with today, so
// OpenAdapter10_2 is the one expected, but the runtime probes the D3D9 entry too on some paths and all three are
// exported so that no probe fails for the wrong reason.
#include <windows.h>
#include <winternl.h>       // the user-mode NTSTATUS: d3dkmddi.h, which d3d10umddi.h pulls in, is declared with it

#define D3D_UMD_INTERFACE_VERSION D3D_UMD_INTERFACE_VERSION_WDDM2_0
#define D3D10DDI_MINOR_HEADER_VERSION 2
#define D3D11DDI_MINOR_HEADER_VERSION 1

#include <d3d9types.h>      // d3dumddi.h's D3D9 half is built on D3DMATRIX and friends and does not include them
#include <d3dumddi.h>
#include <d3d10umddi.h>

// Which entry point the runtime asks for, and in which order, is a stage A measurement, and E_NOTIMPL on its own
// leaves no trace anywhere. OutputDebugString is the one channel a user-mode DLL has that needs no file, no
// registry value, no resident process and no privilege; with nothing listening it costs a swallowed exception.
static void Note(const char* Text)
{
    OutputDebugStringA(Text);
}

// E_NOTIMPL, not a crash and not a silent success: the runtime is told plainly that this adapter cannot render.
// Returning S_OK with an empty function table would put the runtime a long way into a stack that is not there.
HRESULT APIENTRY OpenAdapter(_Inout_ D3DDDIARG_OPENADAPTER* pOpenData)
{
    UNREFERENCED_PARAMETER(pOpenData);
    Note("bc250umd: OpenAdapter (D3D9) -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

HRESULT APIENTRY OpenAdapter10(_Inout_ D3D10DDIARG_OPENADAPTER* pOpenData)
{
    UNREFERENCED_PARAMETER(pOpenData);
    Note("bc250umd: OpenAdapter10 (D3D10) -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

HRESULT APIENTRY OpenAdapter10_2(_Inout_ D3D10DDIARG_OPENADAPTER* pOpenData)
{
    UNREFERENCED_PARAMETER(pOpenData);
    Note("bc250umd: OpenAdapter10_2 (D3D11) -> E_NOTIMPL\n");
    return E_NOTIMPL;
}

// The prototypes above are checked against the runtime's own function pointer types, so that a signature that has
// drifted is a build error here rather than a stack mismatch in the lab.
static const PFND3DDDI_OPENADAPTER g_CheckD3D9 = OpenAdapter;
static const PFND3D10DDI_OPENADAPTER g_CheckD3D10 = OpenAdapter10;
static const PFND3D10DDI_OPENADAPTER g_CheckD3D11 = OpenAdapter10_2;

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);
    UNREFERENCED_PARAMETER(g_CheckD3D9);
    UNREFERENCED_PARAMETER(g_CheckD3D10);
    UNREFERENCED_PARAMETER(g_CheckD3D11);
    if (Reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(Instance);
    return TRUE;
}
