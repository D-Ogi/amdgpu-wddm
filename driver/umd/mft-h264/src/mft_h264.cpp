#include "mft_h264.h"
#include <new>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Diagnostic trace of the asynchronous event handshake, off unless BC250_MFT_TRACE is set in the
// environment. It names the calling thread, so the order of ProcessInput, ProcessOutput and the
// queued events is readable across the client's worker threads. Written straight to the standard
// error handle: no CRT stream state is shared with a host we are loaded into.
namespace {
int MftTraceOn()
{
    static int state = -1;
    if (state < 0) {
        char buf[8];
        state = (GetEnvironmentVariableA("BC250_MFT_TRACE", buf, sizeof(buf)) > 0) ? 1 : 0;
    }
    return state;
}
void MftTrace(const char* fmt, ...)
{
    if (MftTraceOn() == 0) {
        return;
    }
    char line[256];
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[mft t%05lu] ",
                              static_cast<unsigned long>(GetCurrentThreadId()));
    if (n <= 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int m = _vsnprintf_s(line + n, sizeof(line) - static_cast<size_t>(n), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m <= 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n + m), &written, nullptr);
}
} // namespace

// {A32438F0-0D79-4CA9-A5BF-9F3C80837253}
extern "C" const GUID CLSID_Bc250H264EncoderMFT =
    { 0xa32438f0, 0x0d79, 0x4ca9, { 0xa5, 0xbf, 0x9f, 0x3c, 0x80, 0x83, 0x72, 0x53 } };

static long g_encodedPictures = 0;

extern "C" long __stdcall Bc250H264EncodedPictureCount(void)
{
    return g_encodedPictures;
}

namespace bc250h264 {

namespace {

// The hardware URL is an opaque identifier; the topology loader only tests for its presence (it is
// what makes MFT_ENUM_FLAG_HARDWARE match us). We use a stable string of our own.
const wchar_t kHardwareUrl[] = L"amdgpu_wddm://h264-encoder/0";
const wchar_t kFriendlyName[] = L"BC-250 H.264 Encoder MFT";
const wchar_t kVendorId[] = L"VEN_1002";

// How many bytes the output buffer gets, as a fraction of the uncompressed frame. A key frame at a
// low quantiser can be large; half the raw frame plus a floor is comfortable and still bounded.
uint32_t OutputBufferBytes(uint32_t w, uint32_t h)
{
    const uint64_t raw = static_cast<uint64_t>(w) * h * 3u / 2u;
    uint64_t bytes = raw / 2u + 65536u;
    if (bytes < 262144u) {
        bytes = 262144u;
    }
    return static_cast<uint32_t>(bytes);
}

class Lock {
public:
    explicit Lock(CRITICAL_SECTION* cs) : m_cs(cs) { EnterCriticalSection(m_cs); }
    ~Lock() { LeaveCriticalSection(m_cs); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    CRITICAL_SECTION* m_cs;
};

} // namespace

void Bc250H264Mft::FrameLock::Release()
{
    if (locked2d && buffer2d) {
        buffer2d->Unlock2D();
    }
    if (locked1d && buffer) {
        buffer->Unlock();
    }
    locked2d = false;
    locked1d = false;
    texture.Reset();
    buffer2d.Reset();
    buffer.Reset();
}

Bc250H264Mft::Bc250H264Mft()
{
    InitializeCriticalSection(&m_lock);
    m_lockInit = true;
}

Bc250H264Mft::~Bc250H264Mft()
{
    m_encoder.Shutdown();
    if (m_lockInit) {
        DeleteCriticalSection(&m_lock);
    }
}

HRESULT Bc250H264Mft::Construct()
{
    HRESULT hr = MFCreateAttributes(&m_attributes, 16);
    if (FAILED(hr)) {
        return hr;
    }
    hr = MFCreateEventQueue(&m_events);
    if (FAILED(hr)) {
        return hr;
    }

    // The asynchronous hardware transform contract. A client that does not set
    // MF_TRANSFORM_ASYNC_UNLOCK gets MF_E_TRANSFORM_ASYNC_LOCKED from every streaming call.
    hr = m_attributes->SetUINT32(MF_TRANSFORM_ASYNC, 1);
    if (SUCCEEDED(hr)) { hr = m_attributes->SetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE, 1); }
    if (SUCCEEDED(hr)) { hr = m_attributes->SetUINT32(MF_SA_D3D11_AWARE, 1); }
    if (SUCCEEDED(hr)) { hr = m_attributes->SetUINT32(MF_SA_D3D_AWARE, 1); }
    if (SUCCEEDED(hr)) { hr = m_attributes->SetString(MFT_ENUM_HARDWARE_URL_Attribute, kHardwareUrl); }
    if (SUCCEEDED(hr)) { hr = m_attributes->SetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, kVendorId); }
    if (SUCCEEDED(hr)) { hr = m_attributes->SetString(MFT_FRIENDLY_NAME_Attribute, kFriendlyName); }
    // Merit is only consulted for MFTs that pass a certification check; we claim none.
    if (SUCCEEDED(hr)) { hr = m_attributes->SetUINT32(MFT_CODEC_MERIT_Attribute, 0); }
    if (SUCCEEDED(hr)) { hr = m_attributes->SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_Bc250H264EncoderMFT); }
    return hr;
}

HRESULT Bc250H264Mft::QueryInterface(REFIID riid, void** ppv)
{
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFTransform)) {
        *ppv = static_cast<IMFTransform*>(this);
    } else if (riid == __uuidof(IMFMediaEventGenerator)) {
        *ppv = static_cast<IMFMediaEventGenerator*>(this);
    } else if (riid == __uuidof(IMFShutdown)) {
        *ppv = static_cast<IMFShutdown*>(this);
    } else if (riid == __uuidof(IMFAttributes)) {
        *ppv = static_cast<IMFAttributes*>(this);
    } else if (riid == __uuidof(ICodecAPI)) {
        *ppv = static_cast<ICodecAPI*>(this);
    } else if (riid == __uuidof(IMFRealTimeClientEx)) {
        *ppv = static_cast<IMFRealTimeClientEx*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG Bc250H264Mft::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG Bc250H264Mft::Release()
{
    const LONG n = InterlockedDecrement(&m_refCount);
    if (n == 0) {
        delete this;
    }
    return static_cast<ULONG>(n);
}

HRESULT Bc250H264Mft::CheckValid() const
{
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    UINT32 unlocked = 0;
    if (FAILED(m_attributes->GetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, &unlocked)) || unlocked == 0) {
        return MF_E_TRANSFORM_ASYNC_LOCKED;
    }
    return S_OK;
}

// ---------------------------------------------------------------- stream shape

HRESULT Bc250H264Mft::GetStreamLimits(DWORD* inMin, DWORD* inMax, DWORD* outMin, DWORD* outMax)
{
    if (!inMin || !inMax || !outMin || !outMax) {
        return E_POINTER;
    }
    *inMin = *inMax = *outMin = *outMax = 1;
    return S_OK;
}

HRESULT Bc250H264Mft::GetStreamCount(DWORD* in, DWORD* out)
{
    if (!in || !out) {
        return E_POINTER;
    }
    *in = 1;
    *out = 1;
    return S_OK;
}

HRESULT Bc250H264Mft::GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*)
{
    // Fixed stream identifiers 0 and 0, so the contract says E_NOTIMPL.
    return E_NOTIMPL;
}

HRESULT Bc250H264Mft::GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO* info)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!info) {
        return E_POINTER;
    }
    Lock guard(&m_lock);
    memset(info, 0, sizeof(*info));
    info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
    if (m_inputType) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(m_inputType.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
            const uint32_t bpp = (m_inputSubtype == MFVideoFormat_ARGB32) ? 32u : 12u;
            info->cbSize = w * h * bpp / 8u;
        }
    }
    return S_OK;
}

HRESULT Bc250H264Mft::GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO* info)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!info) {
        return E_POINTER;
    }
    Lock guard(&m_lock);
    memset(info, 0, sizeof(*info));
    // We allocate the output samples ourselves: one access unit per sample, variable length.
    info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
    info->cbSize = OutputBufferBytes(m_cfg.width, m_cfg.height);
    return S_OK;
}

HRESULT Bc250H264Mft::GetAttributes(IMFAttributes** out)
{
    if (!out) {
        return E_POINTER;
    }
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    *out = m_attributes.Get();
    (*out)->AddRef();
    return S_OK;
}

HRESULT Bc250H264Mft::GetInputStreamAttributes(DWORD, IMFAttributes**)
{
    return E_NOTIMPL;
}

HRESULT Bc250H264Mft::GetOutputStreamAttributes(DWORD, IMFAttributes**)
{
    return E_NOTIMPL;
}

HRESULT Bc250H264Mft::DeleteInputStream(DWORD)
{
    return E_NOTIMPL;
}

HRESULT Bc250H264Mft::AddInputStreams(DWORD, DWORD*)
{
    return E_NOTIMPL;
}

// ---------------------------------------------------------------- media types

HRESULT Bc250H264Mft::BuildInputType(DWORD index, IMFMediaType** out) const
{
    static const GUID kSubtypes[] = { MFVideoFormat_NV12, MFVideoFormat_IYUV, MFVideoFormat_ARGB32 };
    if (index >= ARRAYSIZE(kSubtypes)) {
        return MF_E_NO_MORE_TYPES;
    }
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        return hr;
    }
    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) { hr = type->SetGUID(MF_MT_SUBTYPE, kSubtypes[index]); }
    if (SUCCEEDED(hr)) { hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); }
    // The frame size and rate are only advertised once the output type has fixed them; before that
    // the type stays partial, which is what a client expects from an encoder.
    if (SUCCEEDED(hr) && m_outputType) {
        UINT32 w = 0, h = 0, num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeSize(m_outputType.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
            hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
        }
        if (SUCCEEDED(hr) &&
            SUCCEEDED(MFGetAttributeRatio(m_outputType.Get(), MF_MT_FRAME_RATE, &num, &den))) {
            hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, num, den);
        }
    }
    if (FAILED(hr)) {
        return hr;
    }
    *out = type.Detach();
    return S_OK;
}

HRESULT Bc250H264Mft::BuildOutputType(IMFMediaType** out) const
{
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        return hr;
    }
    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) { hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264); }
    if (SUCCEEDED(hr)) { hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); }
    // Constrained Baseline: I and P pictures, CAVLC, no field coding, one reference.
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_MPEG2_PROFILE,
                             static_cast<UINT32>(eAVEncH264VProfile_ConstrainedBase));
    }
    if (SUCCEEDED(hr)) { hr = type->SetUINT32(MF_MT_AVG_BITRATE, m_cfg.meanBitRate); }
    if (SUCCEEDED(hr) && m_inputType) {
        UINT32 w = 0, h = 0, num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeSize(m_inputType.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
            hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
        }
        if (SUCCEEDED(hr) &&
            SUCCEEDED(MFGetAttributeRatio(m_inputType.Get(), MF_MT_FRAME_RATE, &num, &den))) {
            hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, num, den);
        }
    }
    if (FAILED(hr)) {
        return hr;
    }
    *out = type.Detach();
    return S_OK;
}

HRESULT Bc250H264Mft::GetInputAvailableType(DWORD id, DWORD index, IMFMediaType** out)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!out) {
        return E_POINTER;
    }
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    Lock guard(&m_lock);
    return BuildInputType(index, out);
}

HRESULT Bc250H264Mft::GetOutputAvailableType(DWORD id, DWORD index, IMFMediaType** out)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!out) {
        return E_POINTER;
    }
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    if (index != 0) {
        return MF_E_NO_MORE_TYPES;
    }
    Lock guard(&m_lock);
    return BuildOutputType(out);
}

HRESULT Bc250H264Mft::SetInputType(DWORD id, IMFMediaType* type, DWORD flags)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    Lock guard(&m_lock);

    if (type == nullptr) {
        if (flags & MFT_SET_TYPE_TEST_ONLY) {
            return S_OK;
        }
        m_inputType.Reset();
        m_inputSubtype = GUID_NULL;
        m_encoder.Shutdown();
        m_encoderReady = false;
        return S_OK;
    }

    GUID major = GUID_NULL, subtype = GUID_NULL;
    HRESULT hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || major != MFMediaType_Video) {
        return MF_E_INVALIDMEDIATYPE;
    }
    hr = type->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr)) {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (subtype != MFVideoFormat_NV12 && subtype != MFVideoFormat_IYUV &&
        subtype != MFVideoFormat_ARGB32) {
        return MF_E_INVALIDMEDIATYPE;
    }
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || w < 16 || h < 16) {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (m_outputType) {
        UINT32 ow = 0, oh = 0;
        if (SUCCEEDED(MFGetAttributeSize(m_outputType.Get(), MF_MT_FRAME_SIZE, &ow, &oh)) &&
            (ow != w || oh != h)) {
            return MF_E_INVALIDMEDIATYPE;
        }
    }
    UINT32 interlace = MFVideoInterlace_Progressive;
    if (SUCCEEDED(type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace)) &&
        interlace != MFVideoInterlace_Progressive &&
        interlace != MFVideoInterlace_MixedInterlaceOrProgressive) {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (flags & MFT_SET_TYPE_TEST_ONLY) {
        return S_OK;
    }

    m_inputType.Reset();
    m_inputType.CopyFrom(type);
    m_inputSubtype = subtype;
    m_cfg.width = w;
    m_cfg.height = h;
    // A format change restarts the encoder on the next picture.
    m_encoder.Shutdown();
    m_encoderReady = false;
    return S_OK;
}

HRESULT Bc250H264Mft::SetOutputType(DWORD id, IMFMediaType* type, DWORD flags)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    Lock guard(&m_lock);

    if (type == nullptr) {
        if (flags & MFT_SET_TYPE_TEST_ONLY) {
            return S_OK;
        }
        m_outputType.Reset();
        m_encoder.Shutdown();
        m_encoderReady = false;
        return S_OK;
    }

    GUID major = GUID_NULL, subtype = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_H264) {
        return MF_E_INVALIDMEDIATYPE;
    }
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || w < 16 || h < 16) {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (w > 4096 || h > 4096) {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (m_inputType) {
        UINT32 iw = 0, ih = 0;
        if (SUCCEEDED(MFGetAttributeSize(m_inputType.Get(), MF_MT_FRAME_SIZE, &iw, &ih)) &&
            (iw != w || ih != h)) {
            return MF_E_INVALIDMEDIATYPE;
        }
    }
    UINT32 profile = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_MPEG2_PROFILE, &profile)) && profile != 0) {
        // Only the baseline family is implemented: no CABAC, no B pictures, no 8x8 transform.
        if (profile != static_cast<UINT32>(eAVEncH264VProfile_Base) &&
            profile != static_cast<UINT32>(eAVEncH264VProfile_ConstrainedBase) &&
            profile != static_cast<UINT32>(eAVEncH264VProfile_Simple)) {
            return MF_E_INVALIDMEDIATYPE;
        }
    }
    if (flags & MFT_SET_TYPE_TEST_ONLY) {
        return S_OK;
    }

    EncoderConfig cfg = m_cfg;
    cfg.width = w;
    cfg.height = h;
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) && num != 0 && den != 0) {
        cfg.fpsNum = num;
        cfg.fpsDen = den;
    }
    UINT32 bitrate = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_AVG_BITRATE, &bitrate)) && bitrate != 0) {
        cfg.meanBitRate = bitrate;
    }
    UINT32 gop = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_MAX_KEYFRAME_SPACING, &gop)) && gop != 0) {
        cfg.gopSize = gop;
    }
    m_cfg = cfg;

    // The sequence header has to be on the output type before the first sample, because a file sink
    // writes it into the container. MakeSequenceParams needs no device, so this works here.
    SequenceParams sps;
    PictureParams pps;
    MakeSequenceParams(m_cfg, &sps, &pps);
    std::vector<uint8_t> nals;
    BuildParameterSetNals(nals, sps, pps);

    m_outputType.Reset();
    m_outputType.CopyFrom(type);
    HRESULT hr = m_outputType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, nals.data(),
                                      static_cast<UINT32>(nals.size()));
    if (SUCCEEDED(hr)) {
        hr = m_outputType->SetUINT32(MF_MT_MPEG2_LEVEL, sps.levelIdc);
    }
    if (FAILED(hr)) {
        m_outputType.Reset();
        return hr;
    }
    m_encoder.Shutdown();
    m_encoderReady = false;
    return S_OK;
}

HRESULT Bc250H264Mft::GetInputCurrentType(DWORD id, IMFMediaType** out)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!out) {
        return E_POINTER;
    }
    Lock guard(&m_lock);
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    if (!m_inputType) {
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    *out = m_inputType.Get();
    (*out)->AddRef();
    return S_OK;
}

HRESULT Bc250H264Mft::GetOutputCurrentType(DWORD id, IMFMediaType** out)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!out) {
        return E_POINTER;
    }
    Lock guard(&m_lock);
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    if (!m_outputType) {
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    *out = m_outputType.Get();
    (*out)->AddRef();
    return S_OK;
}

HRESULT Bc250H264Mft::GetInputStatus(DWORD id, DWORD* flags)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!flags) {
        return E_POINTER;
    }
    Lock guard(&m_lock);
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    *flags = (m_inputType && m_outputType && !m_pendingOutput) ? MFT_INPUT_STATUS_ACCEPT_DATA : 0;
    return S_OK;
}

HRESULT Bc250H264Mft::GetOutputStatus(DWORD* flags)
{
    if (!flags) {
        return E_POINTER;
    }
    Lock guard(&m_lock);
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    *flags = m_pendingOutput ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
    return S_OK;
}

HRESULT Bc250H264Mft::SetOutputBounds(LONGLONG, LONGLONG)
{
    return E_NOTIMPL;
}

HRESULT Bc250H264Mft::ProcessEvent(DWORD, IMFMediaEvent*)
{
    return E_NOTIMPL;
}

// ---------------------------------------------------------------- encode path

HRESULT Bc250H264Mft::EnsureEncoder()
{
    if (m_encoderReady) {
        return S_OK;
    }
    if (!m_inputType || !m_outputType) {
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    // The device the client gave us through MFT_MESSAGE_SET_D3D_MANAGER, if any. On unit A that is
    // the compositor's or the capture pipeline's own device, created on our driver, so the compute
    // dispatches below run on the BC-250 GPU.
    ComPtr<ID3D11Device> device;
    if (m_deviceManager) {
        HANDLE handle = nullptr;
        HRESULT hr = m_deviceManager->OpenDeviceHandle(&handle);
        if (SUCCEEDED(hr)) {
            hr = m_deviceManager->GetVideoService(handle, __uuidof(ID3D11Device),
                                                  reinterpret_cast<void**>(&device));
            m_deviceManager->CloseDeviceHandle(handle);
        }
        if (FAILED(hr)) {
            return hr;
        }
    }
    HRESULT hr = m_encoder.Initialize(device.Get(), m_cfg);
    if (FAILED(hr)) {
        return hr;
    }
    m_encoderReady = true;
    return S_OK;
}

HRESULT Bc250H264Mft::SampleToFrame(IMFSample* sample, GpuFrameInput* frame, FrameLock* lock)
{
    HRESULT hr = sample->GetBufferByIndex(0, &lock->buffer);
    if (FAILED(hr)) {
        return hr;
    }

    const uint32_t w = m_cfg.width;
    const uint32_t h = m_cfg.height;

    // A Direct3D surface arrives wrapped in an IMFDXGIBuffer. Keeping it on the GPU is the whole
    // point of MF_SA_D3D11_AWARE, so this is the preferred path.
    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(lock->buffer->QueryInterface(__uuidof(IMFDXGIBuffer),
                                               reinterpret_cast<void**>(&dxgi)))) {
        if (m_inputSubtype == MFVideoFormat_IYUV) {
            // No planar YUV texture format exists in D3D11; a client that hands us an I420 surface
            // is wrong about its own type.
            return MF_E_UNSUPPORTED_D3D_TYPE;
        }
        // The array slice the buffer refers to. Game Bar, the Windows frame server and
        // Windows.Graphics.Capture all hand out slices of a texture array, so a non-zero index is
        // the normal case; the GPU import pass copies that slice into a private texture.
        UINT subresource = 0;
        if (FAILED(dxgi->GetSubresourceIndex(&subresource))) {
            subresource = 0;
        }
        hr = dxgi->GetResource(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&lock->texture));
        if (FAILED(hr)) {
            return hr;
        }
        frame->texture = lock->texture.Get();
        frame->slice = subresource;
        frame->kind = (m_inputSubtype == MFVideoFormat_ARGB32) ? InputKind::TextureBGRA
                                                              : InputKind::TextureNV12;
        return S_OK;
    }

    BYTE* base = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer2> b2;
    if (SUCCEEDED(lock->buffer->QueryInterface(__uuidof(IMF2DBuffer2),
                                               reinterpret_cast<void**>(&b2)))) {
        BYTE* start = nullptr;
        DWORD length = 0;
        hr = b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &base, &pitch, &start, &length);
        if (FAILED(hr)) {
            return hr;
        }
        lock->buffer2d = std::move(b2);
        lock->locked2d = true;
    } else {
        DWORD maxLen = 0, curLen = 0;
        hr = lock->buffer->Lock(&base, &maxLen, &curLen);
        if (FAILED(hr)) {
            return hr;
        }
        lock->locked1d = true;
        pitch = static_cast<LONG>((m_inputSubtype == MFVideoFormat_ARGB32) ? w * 4u : w);
    }

    const uint32_t absPitch = static_cast<uint32_t>(pitch < 0 ? -pitch : pitch);
    if (pitch < 0) {
        // Bottom-up buffer (MF hands RGB32 this way). Repack to top-down; the GPU import pass has no
        // flip and silently mirroring the picture would be worse than one memcpy per row.
        const uint32_t rows = (m_inputSubtype == MFVideoFormat_ARGB32) ? h : (h + h / 2u);
        m_repack.resize(static_cast<size_t>(absPitch) * rows);
        for (uint32_t y = 0; y < rows; ++y) {
            memcpy(&m_repack[static_cast<size_t>(y) * absPitch], base + static_cast<ptrdiff_t>(y) * pitch,
                   absPitch);
        }
        base = m_repack.data();
    }

    if (m_inputSubtype == MFVideoFormat_ARGB32) {
        frame->kind = InputKind::BgraSys;
        frame->rgb = base;
        frame->pitchRgb = absPitch;
    } else if (m_inputSubtype == MFVideoFormat_NV12) {
        frame->kind = InputKind::Nv12Sys;
        frame->planeY = base;
        frame->planeCb = base + static_cast<size_t>(absPitch) * h;
        frame->pitchY = absPitch;
        frame->pitchC = absPitch;
    } else {
        frame->kind = InputKind::Planar8;
        frame->planeY = base;
        frame->planeCb = base + static_cast<size_t>(absPitch) * h;
        frame->planeCr = frame->planeCb + static_cast<size_t>(absPitch / 2u) * (h / 2u);
        frame->pitchY = absPitch;
        frame->pitchC = absPitch / 2u;
    }
    return S_OK;
}

HRESULT Bc250H264Mft::QueueNeedInput()
{
    if (m_inputRequested) {
        return S_OK;
    }
    m_inputRequested = true;
    MftTrace("queue NeedInput\n");
    return m_events->QueueEventParamVar(METransformNeedInput, GUID_NULL, S_OK, nullptr);
}

HRESULT Bc250H264Mft::ProcessMessage(MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    MftTrace("ProcessMessage 0x%lX\n", static_cast<unsigned long>(message));
    // SET_D3D_MANAGER is the one message a client may send before unlocking the asynchronous model,
    // because it is part of setting the transform up rather than streaming.
    if (message != MFT_MESSAGE_SET_D3D_MANAGER) {
        HRESULT hr = CheckValid();
        if (FAILED(hr)) {
            return hr;
        }
    }
    Lock guard(&m_lock);

    switch (message) {
    case MFT_MESSAGE_SET_D3D_MANAGER: {
        m_deviceManager.Reset();
        m_encoder.Shutdown();
        m_encoderReady = false;
        if (param != 0) {
            IUnknown* unk = reinterpret_cast<IUnknown*>(param);
            HRESULT hr = unk->QueryInterface(__uuidof(IMFDXGIDeviceManager),
                                             reinterpret_cast<void**>(&m_deviceManager));
            if (FAILED(hr)) {
                return MF_E_UNSUPPORTED_D3D_TYPE;
            }
        }
        return S_OK;
    }
    case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
    case MFT_MESSAGE_NOTIFY_START_OF_STREAM: {
        HRESULT hr = EnsureEncoder();
        if (FAILED(hr)) {
            return hr;
        }
        if (!m_streaming) {
            m_streaming = true;
            m_forceKeyFrame = true;
        }
        // Unconditional: the credit bookkeeping in QueueNeedInput makes a repeated message free,
        // and after a drain this is the only thing that restarts the flow of input.
        return QueueNeedInput();
    }
    case MFT_MESSAGE_NOTIFY_END_STREAMING:
        m_streaming = false;
        m_inputRequested = false;
        m_pendingOutput.Reset();
        return S_OK;
    case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
        return S_OK;
    case MFT_MESSAGE_COMMAND_FLUSH:
        m_pendingOutput.Reset();
        m_forceKeyFrame = true;
        m_inputRequested = false;
        return S_OK;
    case MFT_MESSAGE_COMMAND_DRAIN:
        // We hold no reordering queue, so there is nothing left to push out. If the last output has
        // not been collected yet its METransformHaveOutput is already in the queue, and the drain
        // event after it tells the client there will be no more.
        // The client drops every outstanding input request when it drains, so the credit we
        // believe we hold is gone with it. Without this the next START_OF_STREAM finds the credit
        // still taken, queues nothing, and the client waits for a need-input that never comes.
        m_inputRequested = false;
        MftTrace("queue DrainComplete (pendingOutput=%d)\n", m_pendingOutput ? 1 : 0);
        return m_events->QueueEventParamVar(METransformDrainComplete, GUID_NULL, S_OK, nullptr);
    case MFT_MESSAGE_COMMAND_MARKER:
        return m_events->QueueEventParamVar(METransformMarker, GUID_NULL, S_OK, nullptr);
    default:
        return S_OK;
    }
}

HRESULT Bc250H264Mft::ProcessInput(DWORD id, IMFSample* sample, DWORD flags)
{
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (sample == nullptr) {
        return E_POINTER;
    }
    if (flags != 0) {
        return E_INVALIDARG;
    }
    HRESULT hr = CheckValid();
    if (FAILED(hr)) {
        return hr;
    }
    Lock guard(&m_lock);
    if (!m_inputType || !m_outputType) {
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    if (m_pendingOutput) {
        return MF_E_NOTACCEPTING;
    }
    hr = EnsureEncoder();
    if (FAILED(hr)) {
        return hr;
    }
    m_inputRequested = false;

    DWORD bufferCount = 0;
    hr = sample->GetBufferCount(&bufferCount);
    if (FAILED(hr)) {
        return hr;
    }
    if (bufferCount == 0) {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (bufferCount > 1) {
        // Our input stream declares MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER; a split sample is
        // still cheap to join and refusing it would break a well-behaved source.
        ComPtr<IMFMediaBuffer> joined;
        hr = sample->ConvertToContiguousBuffer(&joined);
        if (FAILED(hr)) {
            return hr;
        }
    }

    bool forceKey = m_forceKeyFrame;
    UINT32 cleanPointRequest = 0;
    if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPointRequest)) &&
        cleanPointRequest != 0) {
        forceKey = true;
    }

    GpuFrameInput frame;
    FrameLock held;
    hr = SampleToFrame(sample, &frame, &held);
    if (FAILED(hr)) {
        return hr;
    }

    // Serialise against the client's own use of the shared device. The documented contract for a
    // D3D11-aware MFT is that it takes this lock around every use of the device.
    HANDLE deviceHandle = nullptr;
    bool deviceLocked = false;
    if (m_deviceManager) {
        if (SUCCEEDED(m_deviceManager->OpenDeviceHandle(&deviceHandle))) {
            if (SUCCEEDED(m_deviceManager->LockDevice(deviceHandle, __uuidof(ID3D11Device),
                                                      nullptr, TRUE))) {
                deviceLocked = true;
            }
        }
    }

    FrameStats stats;
    hr = m_encoder.EncodeFrame(frame, forceKey, m_bitstream, &stats);

    if (deviceLocked) {
        m_deviceManager->UnlockDevice(deviceHandle, FALSE);
    }
    if (deviceHandle != nullptr) {
        m_deviceManager->CloseDeviceHandle(deviceHandle);
    }
    held.Release();
    if (FAILED(hr)) {
        return hr;
    }
    m_forceKeyFrame = false;
    InterlockedIncrement(&g_encodedPictures);

    ComPtr<IMFSample> out;
    hr = MFCreateSample(&out);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFMediaBuffer> outBuf;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(m_bitstream.size()), &outBuf);
    if (FAILED(hr)) {
        return hr;
    }
    BYTE* dst = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = outBuf->Lock(&dst, &maxLen, &curLen);
    if (FAILED(hr)) {
        return hr;
    }
    memcpy(dst, m_bitstream.data(), m_bitstream.size());
    outBuf->Unlock();
    hr = outBuf->SetCurrentLength(static_cast<DWORD>(m_bitstream.size()));
    if (SUCCEEDED(hr)) { hr = out->AddBuffer(outBuf.Get()); }
    if (FAILED(hr)) {
        return hr;
    }

    LONGLONG time = 0;
    if (SUCCEEDED(sample->GetSampleTime(&time))) {
        out->SetSampleTime(time);
    }
    LONGLONG duration = 0;
    if (SUCCEEDED(sample->GetSampleDuration(&duration)) && duration > 0) {
        m_lastDuration = duration;
    } else if (m_cfg.fpsNum != 0) {
        m_lastDuration = static_cast<LONGLONG>(10000000ull) * m_cfg.fpsDen / m_cfg.fpsNum;
    }
    out->SetSampleDuration(m_lastDuration);
    out->SetUINT32(MFSampleExtension_CleanPoint, stats.keyFrame ? 1u : 0u);
    // Picture-level diagnostics, read by our own tests. Unknown attributes are ignored by clients.
    out->SetUINT32(MFSampleExtension_VideoEncodeQP, stats.qp);

    m_pendingOutput = std::move(out);
    MftTrace("queue HaveOutput (%u bytes)\n", static_cast<unsigned>(m_bitstream.size()));
    return m_events->QueueEventParamVar(METransformHaveOutput, GUID_NULL, S_OK, nullptr);
}

HRESULT Bc250H264Mft::ProcessOutput(DWORD flags, DWORD count, MFT_OUTPUT_DATA_BUFFER* buffers,
                                    DWORD* status)
{
    if (flags != 0) {
        return E_INVALIDARG;
    }
    if (count != 1 || buffers == nullptr || status == nullptr) {
        return E_INVALIDARG;
    }
    HRESULT hr = CheckValid();
    if (FAILED(hr)) {
        return hr;
    }
    *status = 0;
    Lock guard(&m_lock);
    if (buffers[0].dwStreamID != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    if (!m_pendingOutput) {
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    buffers[0].pSample = m_pendingOutput.Detach();
    buffers[0].dwStatus = 0;
    buffers[0].pEvents = nullptr;
    MftTrace("ProcessOutput delivered a sample\n");
    // Room for the next picture: ask for it.
    if (m_streaming) {
        QueueNeedInput();
    }
    return S_OK;
}

// ---------------------------------------------------------------- event generator

HRESULT Bc250H264Mft::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    const HRESULT hr = m_events->GetEvent(flags, event);
    MftTrace("GetEvent -> 0x%08lX\n", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT Bc250H264Mft::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    const HRESULT hr = m_events->BeginGetEvent(callback, state);
    MftTrace("BeginGetEvent -> 0x%08lX\n", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT Bc250H264Mft::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    const HRESULT hr = m_events->EndGetEvent(result, event);
    MediaEventType mt = 0;
    if (SUCCEEDED(hr) && event != nullptr && *event != nullptr) {
        (*event)->GetType(&mt);
    }
    MftTrace("EndGetEvent -> 0x%08lX type %lu\n", static_cast<unsigned long>(hr),
             static_cast<unsigned long>(mt));
    return hr;
}

HRESULT Bc250H264Mft::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                 const PROPVARIANT* value)
{
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    return m_events->QueueEventParamVar(type, extendedType, status, value);
}

// ---------------------------------------------------------------- real time client

// A transform in a real time topology is asked to move its own threads onto the topology's
// multithreaded work queue so that the whole capture chain shares one priority class. This transform
// has no thread of its own: ProcessInput encodes synchronously on the caller's thread and the only
// asynchronous object we own is the platform event queue, whose threads the platform already owns.
// So there is nothing to register, and the contract is satisfied by remembering the queue and
// answering S_OK rather than E_NOINTERFACE, which would make the capture engine fall back to the
// non real time path for the whole topology.
HRESULT Bc250H264Mft::RegisterThreadsEx(DWORD* taskIndex, LPCWSTR, LONG basePriority)
{
    if (taskIndex == nullptr) {
        return E_POINTER;
    }
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    m_workItemPriority = basePriority;
    // *taskIndex is in out: a zero index means "allocate one", and we hand back whatever the caller
    // gave us so that it keeps its own task index for the rest of the topology.
    return S_OK;
}

HRESULT Bc250H264Mft::UnregisterThreads()
{
    m_workQueue = MFASYNC_CALLBACK_QUEUE_UNDEFINED;
    m_workItemPriority = 0;
    return S_OK;
}

HRESULT Bc250H264Mft::SetWorkQueueEx(DWORD queueId, LONG basePriority)
{
    if (m_shutdown) {
        return MF_E_SHUTDOWN;
    }
    m_workQueue = queueId;
    m_workItemPriority = basePriority;
    return S_OK;
}

// ---------------------------------------------------------------- shutdown

HRESULT Bc250H264Mft::Shutdown()
{
    Lock guard(&m_lock);
    if (m_shutdown) {
        return S_OK;
    }
    m_shutdown = true;
    m_streaming = false;
    m_inputRequested = false;
    m_pendingOutput.Reset();
    if (m_events) {
        m_events->Shutdown();
    }
    m_encoder.Shutdown();
    m_encoderReady = false;
    m_deviceManager.Reset();
    return S_OK;
}

HRESULT Bc250H264Mft::GetShutdownStatus(MFSHUTDOWN_STATUS* status)
{
    if (!status) {
        return E_POINTER;
    }
    if (!m_shutdown) {
        return MF_E_INVALIDREQUEST;
    }
    *status = MFSHUTDOWN_COMPLETED;
    return S_OK;
}

// ---------------------------------------------------------------- IMFAttributes

HRESULT Bc250H264Mft::GetItem(REFGUID k, PROPVARIANT* v) { return m_attributes->GetItem(k, v); }
HRESULT Bc250H264Mft::GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) { return m_attributes->GetItemType(k, t); }
HRESULT Bc250H264Mft::CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) { return m_attributes->CompareItem(k, v, r); }
HRESULT Bc250H264Mft::Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) { return m_attributes->Compare(o, t, r); }
HRESULT Bc250H264Mft::GetUINT32(REFGUID k, UINT32* v) { return m_attributes->GetUINT32(k, v); }
HRESULT Bc250H264Mft::GetUINT64(REFGUID k, UINT64* v) { return m_attributes->GetUINT64(k, v); }
HRESULT Bc250H264Mft::GetDouble(REFGUID k, double* v) { return m_attributes->GetDouble(k, v); }
HRESULT Bc250H264Mft::GetGUID(REFGUID k, GUID* v) { return m_attributes->GetGUID(k, v); }
HRESULT Bc250H264Mft::GetStringLength(REFGUID k, UINT32* n) { return m_attributes->GetStringLength(k, n); }
HRESULT Bc250H264Mft::GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* used) { return m_attributes->GetString(k, s, n, used); }
HRESULT Bc250H264Mft::GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) { return m_attributes->GetAllocatedString(k, s, n); }
HRESULT Bc250H264Mft::GetBlobSize(REFGUID k, UINT32* n) { return m_attributes->GetBlobSize(k, n); }
HRESULT Bc250H264Mft::GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* used) { return m_attributes->GetBlob(k, b, n, used); }
HRESULT Bc250H264Mft::GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) { return m_attributes->GetAllocatedBlob(k, b, n); }
HRESULT Bc250H264Mft::GetUnknown(REFGUID k, REFIID iid, LPVOID* p) { return m_attributes->GetUnknown(k, iid, p); }
HRESULT Bc250H264Mft::SetItem(REFGUID k, REFPROPVARIANT v) { return m_attributes->SetItem(k, v); }
HRESULT Bc250H264Mft::DeleteItem(REFGUID k) { return m_attributes->DeleteItem(k); }
HRESULT Bc250H264Mft::DeleteAllItems() { return m_attributes->DeleteAllItems(); }
HRESULT Bc250H264Mft::SetUINT64(REFGUID k, UINT64 v) { return m_attributes->SetUINT64(k, v); }
HRESULT Bc250H264Mft::SetDouble(REFGUID k, double v) { return m_attributes->SetDouble(k, v); }
HRESULT Bc250H264Mft::SetGUID(REFGUID k, REFGUID v) { return m_attributes->SetGUID(k, v); }
HRESULT Bc250H264Mft::SetString(REFGUID k, LPCWSTR v) { return m_attributes->SetString(k, v); }
HRESULT Bc250H264Mft::SetBlob(REFGUID k, const UINT8* b, UINT32 n) { return m_attributes->SetBlob(k, b, n); }
HRESULT Bc250H264Mft::SetUnknown(REFGUID k, IUnknown* v) { return m_attributes->SetUnknown(k, v); }
HRESULT Bc250H264Mft::LockStore() { return m_attributes->LockStore(); }
HRESULT Bc250H264Mft::UnlockStore() { return m_attributes->UnlockStore(); }
HRESULT Bc250H264Mft::GetCount(UINT32* n) { return m_attributes->GetCount(n); }
HRESULT Bc250H264Mft::GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) { return m_attributes->GetItemByIndex(i, k, v); }
HRESULT Bc250H264Mft::CopyAllItems(IMFAttributes* d) { return m_attributes->CopyAllItems(d); }

HRESULT Bc250H264Mft::SetUINT32(REFGUID k, UINT32 v)
{
    if (k == MF_TRANSFORM_ASYNC_UNLOCK) {
        m_asyncUnlocked = (v != 0);
    }
    return m_attributes->SetUINT32(k, v);
}

// ---------------------------------------------------------------- ICodecAPI
//
// Only the settings Game Bar and Chromium actually write are live. Everything else answers honestly
// with E_NOTIMPL so that a client's feature probe sees the real surface instead of silent no-ops.

namespace {

bool IsLiveParam(const GUID& p)
{
    return p == CODECAPI_AVEncCommonRateControlMode ||
           p == CODECAPI_AVEncCommonMeanBitRate ||
           p == CODECAPI_AVEncCommonQuality ||
           p == CODECAPI_AVEncMPVGOPSize ||
           p == CODECAPI_AVEncCommonLowLatency ||
           p == CODECAPI_AVLowLatencyMode ||
           p == CODECAPI_AVEncVideoForceKeyFrame ||
           p == CODECAPI_AVEncVideoEncodeQP ||
           p == CODECAPI_AVEncVideoMinQP ||
           p == CODECAPI_AVEncVideoMaxQP ||
           p == CODECAPI_AVEncH264CABACEnable ||
           p == CODECAPI_AVEncMPVDefaultBPictureCount;
}

HRESULT ReadUlong(const VARIANT* v, ULONG* out)
{
    if (v == nullptr) {
        return E_POINTER;
    }
    switch (v->vt) {
    case VT_UI4: *out = v->ulVal; return S_OK;
    case VT_I4:  *out = static_cast<ULONG>(v->lVal); return S_OK;
    case VT_UI8: *out = static_cast<ULONG>(v->ullVal); return S_OK;
    case VT_BOOL: *out = v->boolVal ? 1u : 0u; return S_OK;
    default: return E_INVALIDARG;
    }
}

void WriteUlong(VARIANT* v, ULONG value)
{
    v->vt = VT_UI4;
    v->ulVal = value;
}

} // namespace

HRESULT Bc250H264Mft::IsSupported(const GUID* p)
{
    if (p == nullptr) {
        return E_POINTER;
    }
    return IsLiveParam(*p) ? S_OK : S_FALSE;
}

HRESULT Bc250H264Mft::IsModifiable(const GUID* p)
{
    if (p == nullptr) {
        return E_POINTER;
    }
    if (!IsLiveParam(*p)) {
        return E_INVALIDARG;
    }
    // Everything live can change between pictures, except the two that describe what the bitstream
    // syntax is; those are fixed by the profile we implement.
    if (*p == CODECAPI_AVEncH264CABACEnable || *p == CODECAPI_AVEncMPVDefaultBPictureCount) {
        return S_FALSE;
    }
    return S_OK;
}

HRESULT Bc250H264Mft::GetParameterRange(const GUID* p, VARIANT* lo, VARIANT* hi, VARIANT* step)
{
    if (p == nullptr || lo == nullptr || hi == nullptr || step == nullptr) {
        return E_POINTER;
    }
    if (*p == CODECAPI_AVEncCommonQuality) {
        WriteUlong(lo, 0); WriteUlong(hi, 100); WriteUlong(step, 1);
        return S_OK;
    }
    if (*p == CODECAPI_AVEncCommonMeanBitRate) {
        WriteUlong(lo, 100000); WriteUlong(hi, 60000000); WriteUlong(step, 1000);
        return S_OK;
    }
    if (*p == CODECAPI_AVEncMPVGOPSize) {
        WriteUlong(lo, 1); WriteUlong(hi, 600); WriteUlong(step, 1);
        return S_OK;
    }
    if (*p == CODECAPI_AVEncVideoEncodeQP || *p == CODECAPI_AVEncVideoMinQP ||
        *p == CODECAPI_AVEncVideoMaxQP) {
        WriteUlong(lo, 0); WriteUlong(hi, 51); WriteUlong(step, 1);
        return S_OK;
    }
    return E_NOTIMPL;
}

HRESULT Bc250H264Mft::GetParameterValues(const GUID* p, VARIANT** values, ULONG* count)
{
    if (p == nullptr || values == nullptr || count == nullptr) {
        return E_POINTER;
    }
    if (*p != CODECAPI_AVEncCommonRateControlMode) {
        return E_NOTIMPL;
    }
    VARIANT* v = static_cast<VARIANT*>(CoTaskMemAlloc(sizeof(VARIANT) * 4));
    if (v == nullptr) {
        return E_OUTOFMEMORY;
    }
    WriteUlong(&v[0], eAVEncCommonRateControlMode_CBR);
    WriteUlong(&v[1], eAVEncCommonRateControlMode_PeakConstrainedVBR);
    WriteUlong(&v[2], eAVEncCommonRateControlMode_UnconstrainedVBR);
    WriteUlong(&v[3], eAVEncCommonRateControlMode_Quality);
    *values = v;
    *count = 4;
    return S_OK;
}

HRESULT Bc250H264Mft::GetDefaultValue(const GUID* p, VARIANT* value)
{
    if (p == nullptr || value == nullptr) {
        return E_POINTER;
    }
    EncoderConfig def;
    if (*p == CODECAPI_AVEncCommonRateControlMode) {
        WriteUlong(value, eAVEncCommonRateControlMode_CBR);
    } else if (*p == CODECAPI_AVEncCommonMeanBitRate) {
        WriteUlong(value, def.meanBitRate);
    } else if (*p == CODECAPI_AVEncCommonQuality) {
        WriteUlong(value, def.quality);
    } else if (*p == CODECAPI_AVEncMPVGOPSize) {
        WriteUlong(value, def.gopSize);
    } else if (*p == CODECAPI_AVEncCommonLowLatency || *p == CODECAPI_AVLowLatencyMode) {
        value->vt = VT_BOOL;
        value->boolVal = def.lowLatency ? VARIANT_TRUE : VARIANT_FALSE;
    } else if (*p == CODECAPI_AVEncVideoEncodeQP) {
        WriteUlong(value, def.qpInit);
    } else if (*p == CODECAPI_AVEncVideoMinQP) {
        WriteUlong(value, def.qpMin);
    } else if (*p == CODECAPI_AVEncVideoMaxQP) {
        WriteUlong(value, def.qpMax);
    } else if (*p == CODECAPI_AVEncH264CABACEnable ||
               *p == CODECAPI_AVEncMPVDefaultBPictureCount) {
        WriteUlong(value, 0);
    } else {
        return E_NOTIMPL;
    }
    return S_OK;
}

HRESULT Bc250H264Mft::GetValue(const GUID* p, VARIANT* value)
{
    if (p == nullptr || value == nullptr) {
        return E_POINTER;
    }
    Lock guard(&m_lock);
    const EncoderConfig& c = m_cfg;
    if (*p == CODECAPI_AVEncCommonRateControlMode) {
        WriteUlong(value, static_cast<ULONG>(c.rateControl));
    } else if (*p == CODECAPI_AVEncCommonMeanBitRate) {
        WriteUlong(value, c.meanBitRate);
    } else if (*p == CODECAPI_AVEncCommonQuality) {
        WriteUlong(value, c.quality);
    } else if (*p == CODECAPI_AVEncMPVGOPSize) {
        WriteUlong(value, c.gopSize);
    } else if (*p == CODECAPI_AVEncCommonLowLatency || *p == CODECAPI_AVLowLatencyMode) {
        value->vt = VT_BOOL;
        value->boolVal = c.lowLatency ? VARIANT_TRUE : VARIANT_FALSE;
    } else if (*p == CODECAPI_AVEncVideoEncodeQP) {
        WriteUlong(value, c.qpInit);
    } else if (*p == CODECAPI_AVEncVideoMinQP) {
        WriteUlong(value, c.qpMin);
    } else if (*p == CODECAPI_AVEncVideoMaxQP) {
        WriteUlong(value, c.qpMax);
    } else if (*p == CODECAPI_AVEncH264CABACEnable ||
               *p == CODECAPI_AVEncMPVDefaultBPictureCount) {
        WriteUlong(value, 0);   // Constrained Baseline: CAVLC only, no B pictures.
    } else if (*p == CODECAPI_AVEncVideoForceKeyFrame) {
        WriteUlong(value, m_forceKeyFrame ? 1u : 0u);
    } else {
        return E_NOTIMPL;
    }
    return S_OK;
}

HRESULT Bc250H264Mft::SetValue(const GUID* p, VARIANT* value)
{
    if (p == nullptr || value == nullptr) {
        return E_POINTER;
    }
    ULONG v = 0;
    HRESULT hr = ReadUlong(value, &v);
    if (FAILED(hr)) {
        return hr;
    }
    Lock guard(&m_lock);
    if (*p == CODECAPI_AVEncCommonRateControlMode) {
        if (v > static_cast<ULONG>(eAVEncCommonRateControlMode_Quality)) {
            return E_INVALIDARG;
        }
        m_cfg.rateControl = static_cast<RateControl>(v);
        m_encoder.SetRateControl(m_cfg.rateControl);
    } else if (*p == CODECAPI_AVEncCommonMeanBitRate) {
        if (v == 0) {
            return E_INVALIDARG;
        }
        m_cfg.meanBitRate = v;
        m_encoder.SetMeanBitRate(v);
    } else if (*p == CODECAPI_AVEncCommonQuality) {
        if (v > 100) {
            return E_INVALIDARG;
        }
        m_cfg.quality = v;
        m_encoder.SetQuality(v);
    } else if (*p == CODECAPI_AVEncMPVGOPSize) {
        if (v == 0) {
            return E_INVALIDARG;
        }
        m_cfg.gopSize = v;
        m_encoder.SetGopSize(v);
    } else if (*p == CODECAPI_AVEncCommonLowLatency || *p == CODECAPI_AVLowLatencyMode) {
        m_cfg.lowLatency = (v != 0);
        m_encoder.SetLowLatency(m_cfg.lowLatency);
    } else if (*p == CODECAPI_AVEncVideoForceKeyFrame) {
        m_forceKeyFrame = (v != 0);
    } else if (*p == CODECAPI_AVEncVideoEncodeQP) {
        if (v > 51) {
            return E_INVALIDARG;
        }
        m_cfg.qpInit = v;
        m_encoder.SetQp(v);
    } else if (*p == CODECAPI_AVEncVideoMinQP) {
        if (v > 51) {
            return E_INVALIDARG;
        }
        m_cfg.qpMin = v;
        m_encoder.SetQpRange(m_cfg.qpMin, m_cfg.qpMax);
    } else if (*p == CODECAPI_AVEncVideoMaxQP) {
        if (v > 51) {
            return E_INVALIDARG;
        }
        m_cfg.qpMax = v;
        m_encoder.SetQpRange(m_cfg.qpMin, m_cfg.qpMax);
    } else if (*p == CODECAPI_AVEncH264CABACEnable) {
        // Refusing is the honest answer: our bitstream is CAVLC and a client that needs CABAC has to
        // know it did not get it.
        return (v == 0) ? S_OK : E_INVALIDARG;
    } else if (*p == CODECAPI_AVEncMPVDefaultBPictureCount) {
        return (v == 0) ? S_OK : E_INVALIDARG;
    } else {
        return E_NOTIMPL;
    }
    return S_OK;
}

HRESULT Bc250H264Mft::RegisterForEvent(const GUID*, LONG_PTR) { return E_NOTIMPL; }
HRESULT Bc250H264Mft::UnregisterForEvent(const GUID*) { return E_NOTIMPL; }

HRESULT Bc250H264Mft::SetAllDefaults()
{
    Lock guard(&m_lock);
    const EncoderConfig def;
    const uint32_t w = m_cfg.width, h = m_cfg.height;
    const uint32_t fn = m_cfg.fpsNum, fd = m_cfg.fpsDen;
    m_cfg = def;
    m_cfg.width = w;
    m_cfg.height = h;
    m_cfg.fpsNum = fn;
    m_cfg.fpsDen = fd;
    m_encoder.SetRateControl(m_cfg.rateControl);
    m_encoder.SetMeanBitRate(m_cfg.meanBitRate);
    m_encoder.SetQuality(m_cfg.quality);
    m_encoder.SetGopSize(m_cfg.gopSize);
    m_encoder.SetLowLatency(m_cfg.lowLatency);
    m_encoder.SetQp(m_cfg.qpInit);
    m_encoder.SetQpRange(m_cfg.qpMin, m_cfg.qpMax);
    return S_OK;
}

HRESULT Bc250H264Mft::SetValueWithNotify(const GUID* p, VARIANT* value, GUID** changed,
                                         ULONG* count)
{
    if (changed == nullptr || count == nullptr) {
        return E_POINTER;
    }
    *changed = nullptr;
    *count = 0;
    return SetValue(p, value);
}

HRESULT Bc250H264Mft::SetAllDefaultsWithNotify(GUID** changed, ULONG* count)
{
    if (changed == nullptr || count == nullptr) {
        return E_POINTER;
    }
    *changed = nullptr;
    *count = 0;
    return SetAllDefaults();
}

HRESULT Bc250H264Mft::GetAllSettings(IStream*) { return E_NOTIMPL; }
HRESULT Bc250H264Mft::SetAllSettings(IStream*) { return E_NOTIMPL; }
HRESULT Bc250H264Mft::SetAllSettingsWithNotify(IStream*, GUID**, ULONG*) { return E_NOTIMPL; }

} // namespace bc250h264
