// SPDX-License-Identifier: MIT
//
// M15.14 increment 1: the front's five adapter entries, its device create, its log and its device lookup.
//
// GetSupportedVersions appends `D3D11_1_DDI_SUPPORTED` to what the hosted driver answers. It is appended
// exactly, with build version 0 (d3d10umddi.h:7788), because `IS_DXGI_MULTIPLANE_OVERLAY_FUNCTIONS` needs
// a build version above 0 and a build version of 1 would put the four multiplane-overlay entries into the
// front's obligation for no gain (front-dxgi.cpp).
//
// GetCaps answers the D3D11-era caps types itself. The hosted driver's GetCaps zeroes the output buffer
// and returns S_OK for every type (Adapter.cpp), and at the D3D11 DDI a zeroed `3DPIPELINESUPPORT` says
// "no 3D pipeline level at all", which is a device the runtime cannot create. The front therefore answers
// the types the 11.1 era defines and forwards the older ones, which are the ones the hosted driver's own
// zeroing is right for.
//
// CreateDevice is where the two DDIs meet. `D3D10DDIARG_CREATEDEVICE` is one struct whose device-funcs
// member is a union keyed by `Interface` (d3d10umddi.h:7118), so the front copies the arguments, rewrites
// `Interface` to the D3D10.0 pair the hosted frontend accepts, aims `pDeviceFuncs` at its own storage,
// calls the hosted entry, and then fills the runtime's `p11_1DeviceFuncs` from what came back. The device
// handle the hosted driver gets is the one the runtime passed, unchanged.
//
// AND IT READS `Interface` FIRST. The front appends D3D11_1 to the hosted driver's list and keeps every
// version the hosted driver offered, so the runtime may still create a D3D10.0 or D3D10.1 device from that
// same list ("The runtime passes values to the Interface and Version members of D3D10DDIARG_CREATEDEVICE
// that are based on the DDI version ... that the driver's GetSupportedVersions returned",
// ref/windows-driver-docs/.../display/version-discovery-support.md). The union member then points at a
// table of 101 entries, not 155, and the DXGI block at 7 entries, not 15: filling the 11.1 shapes into them
// writes 432 and 64 bytes past the runtime's own buffers inside dwm.exe, with S_OK as the answer. Both
// entries therefore forward a non-11.1 create to the hosted driver untouched, which is byte for byte the
// route the desktop had before the front, and say so once in the log.

#include "front-resource.h"
#include <cstdarg>
#include <cstdio>

namespace bc250front {
namespace {

// ---------------------------------------------------------------- adapters and devices

// Both tables are tiny and both are written only at create time. A process has one adapter per Direct3D
// adapter object and the compositor has one device; the scans below stop at the first empty slot, so the
// common case is one compare.
const unsigned int kAdapterSlots = 8;
const unsigned int kDeviceSlots = 64;

SRWLOCK StateLock = SRWLOCK_INIT;
Adapter Adapters[kAdapterSlots];
bool AdapterUsed[kAdapterSlots];
struct DeviceSlot {
    void *base;
    Device *record;
};
DeviceSlot Devices[kDeviceSlots];

// The process-wide log, set once by the first Install from the same directory and image name the adapter's
// own log uses. The DXGI entries carry a DXGI_DDI_HDEVICE, which is the runtime's handle and not the one the
// front publishes, so those bodies have no device record and no adapter to reach a log through; without this
// fallback every DXGI refusal and every unknown-device line reached a kernel debugger alone, and live kd on
// the development PC is disabled (CLAUDE.md). A refusal nobody can read is not a witness.
Log ProcessLog;
volatile LONG ProcessLogSet;

const Log *FallbackLog()
{
    return InterlockedCompareExchange(&ProcessLogSet, 1, 1) == 1 ? &ProcessLog : nullptr;
}

}  // namespace

// ---------------------------------------------------------------- the log

void LogLine(const Log *log, const char *text, int bytes)
{
    OutputDebugStringA(text);
    if (!log || !log->directory[0] || bytes <= 0) return;
    wchar_t path[1024];
    if (_snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%ls\\front-%ls-%lu.log", log->directory,
                     log->exe[0] ? log->exe : L"unknown", GetCurrentProcessId()) < 0)
        return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, (DWORD)bytes, &written, nullptr);
    CloseHandle(file);
}

void LogPrintf(const Log *log, const char *format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    if (n < 0) n = (int)strlen(line);
    LogLine(log, line, n);
}

void Refuse(Device *device, const char *entry, volatile LONG *once)
{
    const Log *log = device && device->adapter ? &device->adapter->log : FallbackLog();
    if (device) InterlockedIncrement(&device->refusals);
    if (InterlockedCompareExchange(once, 1, 0) != 0) return;
    LogPrintf(log, "bc250d3d_front refused %s: a 3D pipeline level 10_0 device cannot reach it\n", entry);
}

void Dropped(Device *device, const char *entry, const char *what, volatile LONG *once)
{
    const Log *log = device && device->adapter ? &device->adapter->log : FallbackLog();
    if (InterlockedCompareExchange(once, 1, 0) != 0) return;
    LogPrintf(log, "bc250d3d_front %s dropped %s\n", entry, what);
}

void CreateFailed(Device *device)
{
    if (!device || !device->um || !device->um->pfnSetErrorCb) return;
    device->um->pfnSetErrorCb(device->hRTCoreLayer, E_OUTOFMEMORY);
}

// ---------------------------------------------------------------- the device lookup

Device *DeviceOf(D3D10DDI_HDEVICE device)
{
    void *base = device.pDrvPrivate;
    if (!base) return nullptr;
    // Read without a lock. A slot is published with a release store after its record is complete and
    // cleared with an interlocked store, so a reader sees either a complete record or no record.
    for (unsigned int i = 0; i < kDeviceSlots; ++i) {
        void *slot = *(void *volatile *)&Devices[i].base;
        if (!slot) return nullptr;
        if (slot == base) return Devices[i].record;
    }
    return nullptr;
}

bool PublishDevice(void *hosted_base, Device *record)
{
    if (!hosted_base || !record) return false;
    bool published = false;
    AcquireSRWLockExclusive(&StateLock);
    for (unsigned int i = 0; i < kDeviceSlots; ++i) {
        if (Devices[i].base && Devices[i].base != hosted_base) continue;
        Devices[i].record = record;
        MemoryBarrier();
        InterlockedExchangePointer(&Devices[i].base, hosted_base);
        published = true;
        break;
    }
    ReleaseSRWLockExclusive(&StateLock);
    return published;
}

void RetireDevice(Device *record)
{
    if (!record) return;
    AcquireSRWLockExclusive(&StateLock);
    for (unsigned int i = 0; i < kDeviceSlots; ++i) {
        if (Devices[i].record != record) continue;
        // The slot keeps its place in the scan order; clearing the base alone would stop the scan for
        // every device behind it, so the last used slot is moved into this one instead.
        unsigned int last = i;
        for (unsigned int j = i + 1; j < kDeviceSlots && Devices[j].base; ++j) last = j;
        Devices[i].record = Devices[last].record;
        InterlockedExchangePointer(&Devices[i].base, Devices[last].base);
        if (last != i) {
            Devices[last].record = nullptr;
            InterlockedExchangePointer(&Devices[last].base, nullptr);
        }
        break;
    }
    ReleaseSRWLockExclusive(&StateLock);
    record->magic = 0;
    record->self = nullptr;
}

namespace {

// ---------------------------------------------------------------- GetSupportedVersions

// Under the shared lock, so that a scan cannot read a record an Install or a CloseAdapter is in the middle
// of writing. It does not make a close that races a create safe: the Adapter* handed out here can still be
// zeroed by CloseAdapter afterwards, which is a runtime that closed an adapter while one of its own device
// creates was in flight. One adapter per process is what this route has.
Adapter *AdapterOf(D3D10DDI_HADAPTER adapter)
{
    void *hosted = adapter.pDrvPrivate;
    if (!hosted) return nullptr;
    Adapter *found = nullptr;
    AcquireSRWLockShared(&StateLock);
    for (unsigned int i = 0; i < kAdapterSlots; ++i)
        if (AdapterUsed[i] && Adapters[i].hosted_adapter == hosted) { found = &Adapters[i]; break; }
    ReleaseSRWLockShared(&StateLock);
    return found;
}

HRESULT APIENTRY GetSupportedVersions(D3D10DDI_HADAPTER hAdapter, UINT32 *entries, UINT64 *versions)
{
    Adapter *adapter = AdapterOf(hAdapter);
    if (!adapter || !entries) return E_INVALIDARG;
    // The count query first: ask the hosted driver how many it has, then add one for D3D11_1.
    UINT32 hosted_count = 0;
    HRESULT hr = adapter->hosted.pfnGetSupportedVersions(hAdapter, &hosted_count, nullptr);
    if (FAILED(hr)) return hr;
    const UINT32 total = hosted_count + 1;
    if (!versions) {
        *entries = total;
        return S_OK;
    }
    // The fill query. The protocol the hosted driver uses is "refuse an array that is too short"
    // (Adapter.cpp), and the front keeps it: a short array is E_OUTOFMEMORY and writes nothing.
    if (*entries < total) return E_OUTOFMEMORY;
    UINT32 written = *entries;
    hr = adapter->hosted.pfnGetSupportedVersions(hAdapter, &written, versions);
    if (FAILED(hr)) return hr;
    if (written != hosted_count || written >= total) return E_UNEXPECTED;
    versions[written] = D3D11_1_DDI_SUPPORTED;
    *entries = written + 1;
    LogPrintf(&adapter->log, "bc250d3d_front supported_versions hosted=%u total=%u added=%016llx\n",
              hosted_count, *entries, (unsigned long long)D3D11_1_DDI_SUPPORTED);
    return S_OK;
}

// ---------------------------------------------------------------- GetCaps

// Writes `value` into the caps buffer when the buffer is exactly the size of the type. A buffer of
// another size is a different runtime's view of the structure, and zeroing part of it would publish caps
// nobody can read: E_INVALIDARG says so instead.
template <typename T>
HRESULT WriteCaps(const D3D10_2DDIARG_GETCAPS *args, const T &value)
{
    if (!args->pData || args->DataSize != sizeof(T)) return E_INVALIDARG;
    memcpy(args->pData, &value, sizeof(T));
    return S_OK;
}

HRESULT APIENTRY GetCaps(D3D10DDI_HADAPTER hAdapter, const D3D10_2DDIARG_GETCAPS *args)
{
    Adapter *adapter = AdapterOf(hAdapter);
    if (!adapter || !args) return E_INVALIDARG;
    switch (args->Type) {
    case D3D11DDICAPS_THREADING: {
        // Not free threaded, so no command lists and no deferred contexts. Every command-list entry in
        // the front's device table refuses, and this is the cap that says the runtime must not call them.
        D3D11DDI_THREADING_CAPS caps;
        caps.Caps = 0;
        return WriteCaps(args, caps);
    }
    case D3D11DDICAPS_SHADER: {
        // No doubles, no shader-model 4.x compute, no debuggable shaders.
        D3D11DDI_SHADER_CAPS caps;
        caps.Caps = 0;
        return WriteCaps(args, caps);
    }
    case D3D11DDICAPS_3DPIPELINESUPPORT: {
        // The one cap the whole front turns on: the pipeline level the hosted frontend really has. The
        // runtime creates the device with this level in D3D10DDIARG_CREATEDEVICE.Flags, and every entry
        // the front refuses is one that a device at this level cannot reach.
        D3D11DDI_3DPIPELINESUPPORT_CAPS caps;
        caps.Caps = D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0);
        return WriteCaps(args, caps);
    }
    case D3D11_1DDICAPS_D3D11_OPTIONS: {
        // No output-merger logic op (the D3D10.0 blend state has no field for one) and no debug binary.
        D3D11_1DDI_D3D11_OPTIONS_DATA caps;
        caps.OutputMergerLogicOp = FALSE;
        caps.AssignDebugBinarySupport = FALSE;
        return WriteCaps(args, caps);
    }
    case D3D11_1DDICAPS_ARCHITECTURE_INFO: {
        // Not a tile-based deferred renderer: this part has a raster pipeline with a local frame buffer.
        D3D11_1DDI_ARCHITECTURE_INFO_DATA caps;
        caps.TileBasedDeferredRenderer = FALSE;
        return WriteCaps(args, caps);
    }
    case D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT: {
        // No reduced-precision shader arithmetic is published: the hosted frontend compiles at full
        // precision and nothing in the path declares 10-bit or 16-bit minimum precision.
        D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA caps;
        caps.PixelShaderMinPrecision = 0;
        caps.AllOtherStagesMinPrecision = 0;
        return WriteCaps(args, caps);
    }
    default:
        break;
    }
    // Everything below the D3D11 block is a legacy D3DDDICAPS_* type (the enum reserves 0 to 127 for
    // them), and the hosted driver's zeroing answer is the right one for those.
    if (args->Type < D3D11DDICAPS_THREADING) return adapter->hosted.pfnGetCaps(hAdapter, args);
    // A type at or above the D3D11 block that the front does not know is answered the way the hosted driver
    // answered every type before the front existed: a zeroed buffer and S_OK. Every one of these caps
    // structures means "unsupported" when zeroed, and the failure mode of the alternative is not a refused
    // cap - `GetCaps` is called while the runtime evaluates the adapter, so an E_INVALIDARG there is a device
    // create that does not happen inside dwm.exe, with the route log still saying front=on. The six types an
    // 11_1 runtime asks for are all answered above; this is the clause for the one a Windows update adds.
    // The line names the type, so a trial can see that the front was asked something it does not model.
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        LogPrintf(&adapter->log, "bc250d3d_front get_caps zeroed unknown type=%d size=%u\n", (int)args->Type,
                  args->DataSize);
    if (!args->pData || !args->DataSize) return E_INVALIDARG;
    ZeroMemory(args->pData, args->DataSize);
    return S_OK;
}

// ---------------------------------------------------------------- CalcPrivateDeviceSize, CreateDevice

// The front's record goes after the hosted driver's block, so the device handle the hosted driver and
// every field-copied entry receive is the one the runtime passed. The alignment keeps the record aligned
// for its own members whatever size the hosted block has.
SIZE_T FrontOffset(SIZE_T hosted_size)
{
    const SIZE_T align = sizeof(void *) * 2;
    return (hosted_size + align - 1) & ~(align - 1);
}

// The one interface version the front publishes a table for. Everything else the runtime may pick from
// GetSupportedVersions belongs to the hosted driver alone.
bool FrontInterface(unsigned int interface_version)
{
    return interface_version == D3D11_1_DDI_INTERFACE_VERSION;
}

SIZE_T APIENTRY CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,
                                      const D3D10DDIARG_CALCPRIVATEDEVICESIZE *args)
{
    Adapter *adapter = AdapterOf(hAdapter);
    if (!adapter || !args) return 0;
    // A create the front does not front: the hosted driver answers the runtime's own question, unchanged.
    // CreateDevice forwards the same create, so the two cannot disagree about the block's layout.
    if (!FrontInterface(args->Interface)) {
        static volatile LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            LogPrintf(&adapter->log,
                      "bc250d3d_front calc_private_device_size forwarded interface=%08x: not D3D11_1\n",
                      args->Interface);
        return adapter->hosted.pfnCalcPrivateDeviceSize(hAdapter, args);
    }
    // The hosted driver is asked with the D3D10.0 interface, because that is the device it will create.
    D3D10DDIARG_CALCPRIVATEDEVICESIZE hosted_args = *args;
    hosted_args.Interface = D3D10_0_DDI_INTERFACE_VERSION;
    const SIZE_T hosted = adapter->hosted.pfnCalcPrivateDeviceSize(hAdapter, &hosted_args);
    if (!hosted) return 0;
    // Remembered for the comparison in CreateDevice, with the inputs that decided it.
    AcquireSRWLockExclusive(&StateLock);
    adapter->hosted_device_size = hosted;
    adapter->size_version = args->Version;
    adapter->size_flags = args->Flags;
    adapter->size_valid = 1;
    ReleaseSRWLockExclusive(&StateLock);
    return FrontOffset(hosted) + sizeof(Device);
}

HRESULT APIENTRY CreateDevice(D3D10DDI_HADAPTER hAdapter, D3D10DDIARG_CREATEDEVICE *args)
{
    Adapter *adapter = AdapterOf(hAdapter);
    if (!adapter || !args) return E_INVALIDARG;
    // The version check comes before everything, because every write below is a D3D11_1 shape. A D3D10.0 or
    // D3D10.1 create is forwarded exactly as the runtime wrote it: the union member it filled is a table of
    // 101 entries and its DXGI block has 7, and the front publishes neither. No front record is created for
    // such a device, so every entry of it is the hosted driver's own and the route is the one that shipped.
    if (!FrontInterface(args->Interface)) {
        static volatile LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            LogPrintf(&adapter->log,
                      "bc250d3d_front create_device forwarded interface=%08x version=%08x: not D3D11_1, the "
                      "hosted driver's own table reaches the runtime\n",
                      args->Interface, args->Version);
        return adapter->hosted.pfnCreateDevice(hAdapter, args);
    }
    if (!args->hDrvDevice.pDrvPrivate || !args->p11_1DeviceFuncs) return E_INVALIDARG;
    // Where the front's record goes. The hosted private size is recomputed from the create arguments
    // rather than remembered from CalcPrivateDeviceSize, so two devices created at once cannot swap
    // offsets.
    D3D10DDIARG_CALCPRIVATEDEVICESIZE size_args;
    ZeroMemory(&size_args, sizeof(size_args));
    size_args.Interface = D3D10_0_DDI_INTERFACE_VERSION;
    size_args.Version = args->Version;
    size_args.Flags = args->Flags;
    const SIZE_T hosted_size = adapter->hosted.pfnCalcPrivateDeviceSize(hAdapter, &size_args);
    if (!hosted_size) return E_FAIL;
    // And it must be the size the runtime sized this block from. Compared for the same (Version, Flags)
    // pair alone: another pair is another device's query and says nothing about this one.
    if (adapter->size_valid && adapter->size_version == args->Version &&
        adapter->size_flags == args->Flags && adapter->hosted_device_size != hosted_size) {
        LogPrintf(&adapter->log,
                  "bc250d3d_front create_device refused: the hosted private size moved from %Iu to %Iu for "
                  "version=%08x flags=%08x\n",
                  adapter->hosted_device_size, hosted_size, args->Version, args->Flags);
        return E_FAIL;
    }
    Device *record = (Device *)((BYTE *)args->hDrvDevice.pDrvPrivate + FrontOffset(hosted_size));
    ZeroMemory(record, sizeof(*record));
    record->magic = kDeviceMagic;
    record->self = record;
    record->adapter = adapter;
    record->um = args->pUMCallbacks;
    record->hRTDevice = args->hRTDevice;
    record->hRTCoreLayer = args->hRTCoreLayer;
    record->interface_version = args->Interface;
    record->create_flags = args->Flags;
    // The hosted create. Everything the runtime passed is kept except the interface version and the
    // device-funcs pointer: the hosted frontend accepts the D3D10.0 pair alone (Device.cpp) and must fill
    // the front's own storage, not the runtime's 11.1 table.
    // `Version` is the RUNTIME's version and is left exactly as it arrived: the hosted frontend ignores it
    // (Device.cpp reads Interface alone) and a rewritten runtime version would be a lie about the caller.
    // `Flags` likewise. Its 11-era bits (SINGLETHREADED 0x10, DEBUGGABLE 0x20, ENABLE_INCREASED_PRIORITY
    // 0x1000) have no meaning at the D3D10.0 DDI, and the pipeline-level bits are 0 at level 10_0, so there
    // is nothing a mask would remove that the hosted frontend reads. It is forwarded whole rather than
    // filtered, because a filter is a promise about which bits the callee looks at.
    D3D10DDIARG_CREATEDEVICE hosted_args = *args;
    hosted_args.Interface = D3D10_0_DDI_INTERFACE_VERSION;
    hosted_args.pDeviceFuncs = &record->hosted;
    const HRESULT hr = adapter->hosted.pfnCreateDevice(hAdapter, &hosted_args);
    if (FAILED(hr)) {
        LogPrintf(&adapter->log, "bc250d3d_front create_device hosted_hr=%08lx\n", (unsigned long)hr);
        return hr;
    }
    // Everything the hosted create wrote back into the shared parts of the argument struct. The DXGI
    // block is the interesting one: the hosted driver wrote its seven entries into the first seven slots
    // of the runtime's DXGI1_2 table through the union member, and the front reads them back before it
    // fills the rest.
    args->hDrvDevice = hosted_args.hDrvDevice;
    // The sub-object retrieval entry is the hosted driver's, unwrapped. It is the one door in the create
    // arguments the front does not account for, and an 11-era request (a video device or context table, a
    // debug interface) would reach code that has only ever answered 10.0-era ones. The front does not wrap
    // it: the request types the hosted frontend answers are its own business and a wrapper that guessed them
    // would refuse what works today. The log says whether it is there at all, so a trial can see the door.
    args->ppfnRetrieveSubObject = hosted_args.ppfnRetrieveSubObject;
    bool hosted_resolve = false;
    if (args->DXGIBaseDDI.pDXGIDDIBaseFunctions3) {
        memcpy(&record->hosted_dxgi, args->DXGIBaseDDI.pDXGIDDIBaseFunctions3,
               sizeof(record->hosted_dxgi));
        hosted_resolve = args->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnResolveSharedResource != nullptr;
        FillDxgiFuncs(args->DXGIBaseDDI.pDXGIDDIBaseFunctions3, record->hosted_dxgi);
    }
    if (!PublishDevice(args->hDrvDevice.pDrvPrivate, record)) {
        // No slot left. The front cannot answer a single entry for this device, so the device must not be
        // created at all: a table whose thunks cannot find their record is worse than a failed create.
        LogPrintf(&adapter->log, "bc250d3d_front create_device refused: no device slot left\n");
        record->hosted.pfnDestroyDevice(args->hDrvDevice);
        return E_OUTOFMEMORY;
    }
    FillDeviceFuncs(args->p11_1DeviceFuncs, record->hosted);
    // `version` and `dxgi_resolve` are here because they are the two inputs of the hosted driver's own DXGI
    // table shape (IS_DXGI1_1_BASE_FUNCTIONS reads the runtime's version), and increment 2 needs to know
    // which of them this lab really sees. `retrieve_subobject` names the one entry the front forwards blind.
    LogPrintf(&adapter->log,
              "bc250d3d_front create_device device=%p interface=%08x version=%08x flags=%08x "
              "pipeline_level=%d hosted_size=%Iu front_offset=%Iu dxgi=%u dxgi_resolve=%s "
              "retrieve_subobject=%u hosted_hr=%08lx\n",
              args->hDrvDevice.pDrvPrivate, args->Interface, args->Version, args->Flags,
              (int)D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(args->Flags), hosted_size,
              FrontOffset(hosted_size), args->DXGIBaseDDI.pDXGIDDIBaseFunctions3 ? 1u : 0u,
              hosted_resolve ? "hosted" : "front",
              args->ppfnRetrieveSubObject && *args->ppfnRetrieveSubObject ? 1u : 0u, (unsigned long)hr);
    // The hosted code, not S_OK. The Mesa frontend answers DXGI_STATUS_NO_REDIRECTION here (Device.cpp), a
    // SUCCESS code that tells DXGI not to use the shared-resource presentation path with the compositor, and
    // that is the behaviour this desktop was measured with. A front that flattened it to S_OK would change
    // the present path of the whole desktop while claiming to change nothing.
    return hr;
}

HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER hAdapter)
{
    Adapter *adapter = AdapterOf(hAdapter);
    if (!adapter) return E_INVALIDARG;
    const PFND3D10DDI_CLOSEADAPTER hosted = adapter->hosted.pfnCloseAdapter;
    AcquireSRWLockExclusive(&StateLock);
    for (unsigned int i = 0; i < kAdapterSlots; ++i) {
        if (&Adapters[i] != adapter) continue;
        ZeroMemory(&Adapters[i], sizeof(Adapters[i]));
        AdapterUsed[i] = false;
        break;
    }
    ReleaseSRWLockExclusive(&StateLock);
    return hosted(hAdapter);
}

}  // namespace

// ---------------------------------------------------------------- the install

Adapter *Install(D3D10DDIARG_OPENADAPTER *args, const wchar_t *log_directory, const wchar_t *exe)
{
    if (!args || !args->pAdapterFuncs_2 || !args->hAdapter.pDrvPrivate) return nullptr;
    const D3D10_2DDI_ADAPTERFUNCS &hosted = *args->pAdapterFuncs_2;
    // Every one of the five must be there: the front calls four of them and forwards the fifth.
    if (!hosted.pfnCalcPrivateDeviceSize || !hosted.pfnCreateDevice || !hosted.pfnCloseAdapter ||
        !hosted.pfnGetSupportedVersions || !hosted.pfnGetCaps)
        return nullptr;
    Adapter *adapter = nullptr;
    AcquireSRWLockExclusive(&StateLock);
    // The slot is filled first and published afterwards: AdapterUsed[i] is what AdapterOf scans, and a slot
    // that said "used" before its record was written would be a hosted table of null entries to any thread
    // that found it. Both sides hold the lock today; the order costs nothing and does not depend on that.
    for (unsigned int i = 0; i < kAdapterSlots; ++i) {
        if (AdapterUsed[i]) continue;
        ZeroMemory(&Adapters[i], sizeof(Adapters[i]));
        Adapters[i].hosted_adapter = args->hAdapter.pDrvPrivate;
        Adapters[i].hosted = hosted;
        if (log_directory) wcsncpy_s(Adapters[i].log.directory, log_directory, _TRUNCATE);
        if (exe) wcsncpy_s(Adapters[i].log.exe, exe, _TRUNCATE);
        AdapterUsed[i] = true;
        adapter = &Adapters[i];
        break;
    }
    // The process-wide fallback log, for the bodies that have no device record to reach a log through.
    if (adapter && !ProcessLogSet) {
        ProcessLog = adapter->log;
        InterlockedExchange(&ProcessLogSet, 1);
    }
    ReleaseSRWLockExclusive(&StateLock);
    if (!adapter) return nullptr;
    args->pAdapterFuncs_2->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
    args->pAdapterFuncs_2->pfnCreateDevice = CreateDevice;
    args->pAdapterFuncs_2->pfnCloseAdapter = CloseAdapter;
    args->pAdapterFuncs_2->pfnGetSupportedVersions = GetSupportedVersions;
    args->pAdapterFuncs_2->pfnGetCaps = GetCaps;
    LogPrintf(&adapter->log, "bc250d3d_front installed adapter=%p interface=%08x version=%08x\n",
              args->hAdapter.pDrvPrivate, args->Interface, args->Version);
    return adapter;
}

}  // namespace bc250front
