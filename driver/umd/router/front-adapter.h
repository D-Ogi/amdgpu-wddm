// SPDX-License-Identifier: MIT
//
// M15.14 increment 1: the D3D11_1 front of bc250d3d_router.dll.
//
// WHY THE FRONT EXISTS. `pfnCheckDirectFlipSupport` is the one place the operating system asks a driver
// whether an application's swap-chain buffer may be scanned out in place of the compositor's own. The
// entry exists in `D3D11_1DDI_DEVICEFUNCS` and in the WDDM2_x tables only (d3d10umddi.h:2992). The
// compositor's user-mode driver on this lab is the hosted Mesa frontend, which negotiates the D3D10.0
// DDI and therefore has no slot for the question: the answer is not FALSE, there is nothing to call.
// The front puts a D3D11_1 table in front of that frontend. It creates the hosted device at the D3D10.0
// DDI behind itself, forwards what the two tables share, translates what grew, refuses what a 10_0
// pipeline cannot reach, and answers the DirectFlip question itself. The hosted driver is not rebuilt
// and not changed: `bc250d3d_zink.dll` stays the binary that was measured to render this desktop.
//
// WHAT THE ANSWER IS. Increment 1 computed the rule of `front-direct-flip.h` for its log line and then
// wrote FALSE. From increment 2 `pfnCheckDirectFlipSupport` writes the rule's own answer: it reads the
// kernel driver's scan-out caps trailer through the adapter query at every call (`ReadScanoutCaps`), and
// the rule's first clause refuses unless that trailer says this adapter start admits a client flip. The
// front moves no allocation and changes no placement; a TRUE only lets the operating system try the
// flip, and the kernel driver re-derives every fact behind it at SetVidPnSourceAddress.
//
// HOW THE FRONT IS REACHED. `DirectFlipFront`, a REG_DWORD under the router's `DesktopRouter` key.
// An absent value means on from 0.7.213.100-tester.15, where the front became a shipped feature. Zero
// means off, and off is byte for byte the router that shipped; so does any other value kind.
// The value is read per `OpenAdapter`, so it is start-latched for `dwm.exe`: a change needs a Windows
// restart, which is also what a new compositor process needs (owner, 2026-10-03, after BD-060).
//
// HOW THE HANDLES STAY IDENTICAL. The front's per-device record lives in the device private block the
// runtime allocates, placed AFTER the hosted driver's own block. `hDrvDevice` is therefore the same
// pointer for the front and for the hosted driver, and an entry the front forwards by field copy needs
// no handle translation at all - it is the hosted function itself in the runtime's table. The front
// finds its record from `hDevice` through `Bc250FrontDeviceOf`, which scans a small published table of
// the devices it created; the scan stops at the first empty slot, so the single-device case is one
// compare. A per-resource record cannot use the same scheme, and `front-resource.h` says why.
#ifndef BC250_FRONT_ADAPTER_H
#define BC250_FRONT_ADAPTER_H

#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#pragma warning(pop)
#include "../../contract/bc250_scanout_caps.h"
#include "front-flip-log.h"

namespace bc250front {

// The front's log. One file per process next to the router's route log, plus OutputDebugString for a
// live session. The file is opened and closed per line: a reader on the lab must be able to read it
// while the compositor is running, and a crashed compositor must not lose the last line.
struct Log {
    wchar_t directory[1024];  // empty: OutputDebugString only
    wchar_t exe[260];
};
void LogLine(const Log *log, const char *text, int bytes);
void LogPrintf(const Log *log, const char *format, ...);

// Per-adapter state. One per successful hosted OpenAdapter10_2 that the front took over. The hosted
// adapter table is saved here because the front overwrites the runtime's copy of it.
struct Adapter {
    void *hosted_adapter;                  // hAdapter.pDrvPrivate the hosted driver published
    D3D10_2DDI_ADAPTERFUNCS hosted;        // the five hosted adapter entries, before the front's install
    Log log;
    // The hosted private device size the last CalcPrivateDeviceSize computed, with the two create inputs
    // that decided it. CreateDevice recomputes the size and compares it against this one for the same pair:
    // the front's record sits at that offset inside a block the runtime sized from the earlier answer, so a
    // hosted size that moved between the two calls would put the record past the end of a dwm.exe heap block
    // and the symptom would be a delayed crash with no line in any log.
    SIZE_T hosted_device_size;
    unsigned int size_version, size_flags;
    volatile LONG size_valid;
    // The runtime's adapter query, kept from OpenAdapter for the scan-out caps trailer (M15.14 increment
    // 2). The runtime's adapter handle and its callback stay valid until CloseAdapter. Null when the
    // runtime passed no callback table: every read then answers with a zero trailer.
    D3D10DDI_HRTADAPTER rt_adapter;
    PFND3DDDI_QUERYADAPTERINFOCB query_adapter_info;
};

// M15.14 increment 2: the kernel driver's scan-out caps trailer, read now. The buffer is sized
// BC250_SCANOUT_CAPS_TOTAL, because the kernel driver writes the trailer only into a buffer that holds all
// of it (driver/contract/bc250_scanout_caps.h). `*caps` is zeroed first and stays zero unless the query
// succeeded and every header field of the trailer is right (magic, version, size, a non-zero geometry), so
// a kernel driver without the trailer, a closed switch, a failed query and a torn trailer all read as the
// same zero, which the rule answers "gated". Returns the query's HRESULT, for the log; E_POINTER when
// there is no adapter or no callback.
HRESULT ReadScanoutCaps(const Adapter *adapter, bc250_scanout_caps *caps);
// The decode alone, for the host gate: the trailer of a zero-initialized query buffer of `bytes` bytes.
void DecodeScanoutCaps(const unsigned char *data, size_t bytes, bc250_scanout_caps *caps);

// Per-device record, in the runtime's device private block after the hosted driver's block.
struct Device {
    unsigned int magic;                    // kDeviceMagic
    Device *self;                          // a second witness that the offset was right
    Adapter *adapter;
    D3D10DDI_DEVICEFUNCS hosted;           // the hosted device table: the front's own storage, never moves
    DXGI_DDI_BASE_FUNCTIONS hosted_dxgi;   // the seven entries the hosted driver filled
    const D3D10DDI_CORELAYER_DEVICECALLBACKS *um;  // pfnSetErrorCb lives here
    D3D10DDI_HRTDEVICE hRTDevice;
    // pfnSetErrorCb takes the CORE LAYER handle, not the device one (d3d10umddi.h:6757). Kept because a
    // create entry of this DDI returns VOID: a driver that cannot create the object has no other way to say
    // so, and a runtime that was not told believes the object exists.
    D3D10DDI_HRTCORELAYER hRTCoreLayer;
    unsigned int interface_version;        // what the runtime asked for, before the front rewrote it
    unsigned int create_flags;             // D3D11DDI_CREATEDEVICE_FLAG_*: carries the pipeline level
    // Witnesses. Every one of them is a count, so a trial reads one line and knows what happened.
    volatile LONG direct_flip_calls;
    volatile LONG direct_flip_true;
    FlipLog flip_log;                      // CheckDirectFlipSupport's change lines and counters (increment 3)
    volatile LONG clear_view_calls;
    volatile LONG clear_view_rect_calls;   // amendment 4: the pass condition is that this stays 0
    volatile LONG refusals;                // entries a 10_0 pipeline cannot reach, called anyway
};
static const unsigned int kDeviceMagic = 0x4E463242u;  // "B2FN"

// The front's own adapter entries. Installed over the runtime's D3D10_2DDI_ADAPTERFUNCS after the
// hosted open succeeded; `Install` keeps the hosted five in the Adapter it returns.
//   Install returns 0 when the front cannot take the adapter over (no 10_2 table, no free slot, an
//   incomplete hosted table). The caller then leaves the hosted table alone: the route is unchanged.
Adapter *Install(D3D10DDIARG_OPENADAPTER *args, const wchar_t *log_directory, const wchar_t *exe);

// Device lookup. Published by CreateDevice, cleared by DestroyDevice.
Device *DeviceOf(D3D10DDI_HDEVICE device);
bool PublishDevice(void *hosted_base, Device *record);
void RetireDevice(Device *record);

// The two table fills, pure functions of the hosted table so that a host gate can drive the production
// code rather than a copy of it (tests/test-router.cpp). Every slot of the output is written.
void FillDeviceFuncs(D3D11_1DDI_DEVICEFUNCS *out, const D3D10DDI_DEVICEFUNCS &hosted);
void FillDxgiFuncs(DXGI1_2_DDI_BASE_FUNCTIONS *out, const DXGI_DDI_BASE_FUNCTIONS &hosted);

// The entry counts the two fills cover. 157 and 15 are the counts of the WDK declaration; the device
// table compiles to 155 members here because the two D3D10PSGP members are reserved for system use and
// are not declared unless D3D10PSGP is defined (ref/ddi-display/d3d10umddi.md:29099). FillDeviceFuncs
// writes all 155, and the static assert below fails if a WDK bump changes the shape.
static const size_t kDeviceEntries = 155;
static const size_t kDeviceEntriesWithPsgp = 157;
static const size_t kDxgiEntries = 15;
static_assert(sizeof(D3D11_1DDI_DEVICEFUNCS) == kDeviceEntries * sizeof(void *),
              "D3D11_1DDI_DEVICEFUNCS is no longer 155 function pointers: FillDeviceFuncs must be "
              "revisited entry by entry before the front may publish the table");
static_assert(sizeof(DXGI1_2_DDI_BASE_FUNCTIONS) == kDxgiEntries * sizeof(void *),
              "DXGI1_2_DDI_BASE_FUNCTIONS is no longer 15 function pointers: FillDxgiFuncs must be "
              "revisited entry by entry");
static_assert(sizeof(D3D10DDI_DEVICEFUNCS) == 101 * sizeof(void *),
              "D3D10DDI_DEVICEFUNCS changed shape: every field copy in FillDeviceFuncs must be rechecked");

// A refusal, from an entry a device at 3D pipeline level 10_0 cannot reach. Counted on every call and
// logged on the first one: "the runtime called it at all" is the interesting fact, and a line per call
// would be a flood on a hot path. `once` is the entry's own flag, so the line names the entry; the
// REFUSE_ONCE and DROPPED_ONCE macros in front-device.cpp declare one per call site.
void Refuse(Device *device, const char *entry, volatile LONG *once);
// A semantic the front could not carry down to the D3D10.0 DDI, on a call it still forwarded. Logged on
// the first occurrence of each entry; the call is NOT refused, because a refused draw path is a dead
// desktop and the dropped semantics are ones a 10_0 pipeline does not use.
void Dropped(Device *device, const char *entry, const char *what, volatile LONG *once);
// A create entry that did not create the object. Every pfnCreate* of this DDI returns VOID, so the runtime
// learns of a failure through pfnSetErrorCb alone: told, it treats the handle as invalid and does NOT call
// the matching Destroy* (ref/ddi-display/d3d10umddi.md:745). Not told, it calls that Destroy*, which is a
// field copy straight into the hosted driver over a private block the hosted driver never initialised - a
// wild pointer inside dwm.exe instead of a line in a log. E_OUTOFMEMORY is the one failure the runtime does
// not treat as critical; any other code it reads as device loss.
void CreateFailed(Device *device);

}  // namespace bc250front

#endif
