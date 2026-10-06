// SPDX-License-Identifier: MIT
// The Media Foundation half of the host test: the inbox decoder used as the oracle, our transform
// driven through the asynchronous hardware MFT contract, the comparison against the inbox encoder,
// and a sink writer run that proves a real pipeline can pick our transform up.
//
// Registration here is always MFTRegisterLocal: the transform becomes visible to this process only,
// and nothing is written to the registry.

#include "mfthost.h"
#include <mfreadwrite.h>
#include <stdio.h>
#include <string.h>
#include <new>

extern "C" HRESULT __stdcall Bc250CreateH264EncoderMFT(REFIID riid, void** ppv);
// The module lock of dllmain.cpp, linked into this test. A host asks this before unmapping the DLL,
// so it must answer S_FALSE while any object of ours is alive.
extern "C" HRESULT __stdcall DllCanUnloadNow(void);
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv);

namespace bc250h264 {
namespace test {

namespace {

// wmcodecdsp.h cannot be included here: it defines struct CodecAPIEventData a second time, and
// icodecapi.h (which the transform's own header needs for ICodecAPI) already defines it, with no
// guard on either side. The two class identifiers we want from it are therefore spelled out.
//   CLSID_CMSH264DecoderMFT  wmcodecdsp.h:5817  62CE7E72-4C71-4d20-B15D-452831A87D9D
//   the inbox H264 Encoder MFT, as MFTEnumEx reports it on this machine and on unit A
const GUID kInboxH264DecoderClsid =
    { 0x62ce7e72, 0x4c71, 0x4d20, { 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d } };
const GUID kInboxH264EncoderClsid =
    { 0x6ca50344, 0x051a, 0x4ded, { 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35 } };

void Say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void SayHr(const char* what, HRESULT hr)
{
    Say("%s failed 0x%08lX", what, static_cast<unsigned long>(hr));
}

HRESULT MakeVideoType(const GUID& subtype, uint32_t w, uint32_t h, uint32_t fps,
                      IMFMediaType** out)
{
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) {
        return hr;
    }
    hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) { hr = t->SetGUID(MF_MT_SUBTYPE, subtype); }
    if (SUCCEEDED(hr)) { hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); }
    if (SUCCEEDED(hr)) { hr = MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h); }
    if (SUCCEEDED(hr)) { hr = MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1); }
    if (SUCCEEDED(hr)) { hr = MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); }
    if (FAILED(hr)) {
        return hr;
    }
    *out = t.Detach();
    return S_OK;
}

// A sample over a block of system memory, filled by the caller.
HRESULT MakeMemorySample(const uint8_t* data, size_t size, int64_t timeHns, int64_t durationHns,
                         IMFSample** out)
{
    ComPtr<IMFSample> s;
    HRESULT hr = MFCreateSample(&s);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFMediaBuffer> b;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &b);
    if (FAILED(hr)) {
        return hr;
    }
    BYTE* dst = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = b->Lock(&dst, &maxLen, &curLen);
    if (FAILED(hr)) {
        return hr;
    }
    memcpy(dst, data, size);
    b->Unlock();
    hr = b->SetCurrentLength(static_cast<DWORD>(size));
    if (SUCCEEDED(hr)) { hr = s->AddBuffer(b.Get()); }
    if (SUCCEEDED(hr)) { hr = s->SetSampleTime(timeHns); }
    if (SUCCEEDED(hr)) { hr = s->SetSampleDuration(durationHns); }
    if (FAILED(hr)) {
        return hr;
    }
    *out = s.Detach();
    return S_OK;
}

// I420 to NV12, for the inbox encoder and for the sink writer.
void I420ToNv12(const Picture& p, std::vector<uint8_t>& nv12)
{
    const uint32_t w = p.width, h = p.height;
    nv12.resize(static_cast<size_t>(w) * h * 3u / 2u);
    memcpy(nv12.data(), p.y.data(), static_cast<size_t>(w) * h);
    uint8_t* uv = nv12.data() + static_cast<size_t>(w) * h;
    const uint32_t cw = w / 2, ch = h / 2;
    for (uint32_t y = 0; y < ch; ++y) {
        for (uint32_t x = 0; x < cw; ++x) {
            uv[static_cast<size_t>(y) * w + x * 2u] = p.cb[static_cast<size_t>(y) * cw + x];
            uv[static_cast<size_t>(y) * w + x * 2u + 1u] = p.cr[static_cast<size_t>(y) * cw + x];
        }
    }
}

// Copies one NV12 output buffer of a decoder into an I420 picture of the visible size.
HRESULT Nv12BufferToPicture(IMFMediaBuffer* buffer, uint32_t codedWidth, uint32_t codedHeight,
                            uint32_t visibleWidth, uint32_t visibleHeight, Picture* out)
{
    out->Allocate(visibleWidth, visibleHeight);
    BYTE* base = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer2> b2;
    bool locked2d = false;
    HRESULT hr = buffer->QueryInterface(__uuidof(IMF2DBuffer2), reinterpret_cast<void**>(&b2));
    if (SUCCEEDED(hr)) {
        BYTE* start = nullptr;
        DWORD length = 0;
        hr = b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &base, &pitch, &start, &length);
        if (FAILED(hr)) {
            return hr;
        }
        locked2d = true;
    } else {
        DWORD maxLen = 0, curLen = 0;
        hr = buffer->Lock(&base, &maxLen, &curLen);
        if (FAILED(hr)) {
            return hr;
        }
        pitch = static_cast<LONG>(codedWidth);
    }
    const uint8_t* y = base;
    const uint8_t* uv = base + static_cast<ptrdiff_t>(pitch) * codedHeight;
    for (uint32_t r = 0; r < visibleHeight; ++r) {
        memcpy(&out->y[static_cast<size_t>(r) * visibleWidth], y + static_cast<ptrdiff_t>(r) * pitch,
               visibleWidth);
    }
    const uint32_t cw = visibleWidth / 2, ch = visibleHeight / 2;
    for (uint32_t r = 0; r < ch; ++r) {
        const uint8_t* row = uv + static_cast<ptrdiff_t>(r) * pitch;
        for (uint32_t c = 0; c < cw; ++c) {
            out->cb[static_cast<size_t>(r) * cw + c] = row[c * 2u];
            out->cr[static_cast<size_t>(r) * cw + c] = row[c * 2u + 1u];
        }
    }
    if (locked2d) {
        b2->Unlock2D();
    } else {
        buffer->Unlock();
    }
    return S_OK;
}

} // namespace

// ---------------------------------------------------------------- the decoder oracle

HRESULT H264Decoder::Initialize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    m_pictures.clear();
    HRESULT hr = CoCreateInstance(kInboxH264DecoderClsid, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMFTransform), reinterpret_cast<void**>(&m_mft));
    if (FAILED(hr)) {
        m_lastError = L"CoCreateInstance(CLSID_CMSH264DecoderMFT)";
        return hr;
    }
    ComPtr<IMFMediaType> in;
    hr = MakeVideoType(MFVideoFormat_H264, width, height, 30, &in);
    if (FAILED(hr)) {
        return hr;
    }
    hr = m_mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        m_lastError = L"SetInputType(H264)";
        return hr;
    }
    // Pick NV12 out of what the decoder offers.
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        hr = m_mft->GetOutputAvailableType(0, i, &t);
        if (hr == MF_E_NO_MORE_TYPES) {
            m_lastError = L"the decoder offers no NV12 output";
            return MF_E_INVALIDMEDIATYPE;
        }
        if (FAILED(hr)) {
            return hr;
        }
        GUID sub = GUID_NULL;
        if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
            hr = m_mft->SetOutputType(0, t.Get(), 0);
            if (FAILED(hr)) {
                m_lastError = L"SetOutputType(NV12)";
                return hr;
            }
            break;
        }
    }
    MFT_OUTPUT_STREAM_INFO osi = {};
    hr = m_mft->GetOutputStreamInfo(0, &osi);
    if (FAILED(hr)) {
        return hr;
    }
    m_providesSamples = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    m_outputBytes = osi.cbSize ? osi.cbSize : (width * height * 3u / 2u + 4096u);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return S_OK;
}

void H264Decoder::Shutdown()
{
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_mft.Reset();
    }
}

HRESULT H264Decoder::PullAll()
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER buf = {};
        ComPtr<IMFSample> allocated;
        if (!m_providesSamples) {
            HRESULT hr = MFCreateSample(&allocated);
            if (FAILED(hr)) {
                return hr;
            }
            ComPtr<IMFMediaBuffer> b;
            hr = MFCreateMemoryBuffer(m_outputBytes, &b);
            if (FAILED(hr)) {
                return hr;
            }
            hr = allocated->AddBuffer(b.Get());
            if (FAILED(hr)) {
                return hr;
            }
            buf.pSample = allocated.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &buf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return S_OK;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The decoder learned the real frame size from the stream: take its new NV12 type.
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> t;
                HRESULT h2 = m_mft->GetOutputAvailableType(0, i, &t);
                if (FAILED(h2)) {
                    m_lastError = L"no NV12 type after a stream change";
                    return h2;
                }
                GUID sub = GUID_NULL;
                if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                    h2 = m_mft->SetOutputType(0, t.Get(), 0);
                    if (FAILED(h2)) {
                        return h2;
                    }
                    break;
                }
            }
            MFT_OUTPUT_STREAM_INFO osi = {};
            m_mft->GetOutputStreamInfo(0, &osi);
            if (osi.cbSize != 0) {
                m_outputBytes = osi.cbSize;
            }
            continue;
        }
        if (FAILED(hr)) {
            m_lastError = L"ProcessOutput";
            return hr;
        }
        // A sample we allocated is still owned by `allocated`: taking it over with Attach released it
        // twice, a use after free that ended a 1080p 600-picture run at picture 421 on unit A (a pure
        // virtual call in the freed sample). Only a sample the decoder provided carries a reference of
        // its own.
        ComPtr<IMFSample> sample;
        if (buf.pSample == allocated.Get()) {
            sample.CopyFrom(buf.pSample);
        } else {
            sample.Attach(buf.pSample);
        }
        if (buf.pEvents != nullptr) {
            buf.pEvents->Release();
        }
        if (!sample) {
            continue;
        }
        ComPtr<IMFMediaType> cur;
        uint32_t cw = m_width, ch = m_height;
        if (SUCCEEDED(m_mft->GetOutputCurrentType(0, &cur))) {
            UINT32 w = 0, h = 0;
            if (SUCCEEDED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
                cw = w;
                ch = h;
            }
        }
        ComPtr<IMFMediaBuffer> b;
        hr = sample->ConvertToContiguousBuffer(&b);
        if (FAILED(hr)) {
            return hr;
        }
        Picture p;
        hr = Nv12BufferToPicture(b.Get(), cw, ch, m_width, m_height, &p);
        if (FAILED(hr)) {
            m_lastError = L"Nv12BufferToPicture";
            return hr;
        }
        m_pictures.push_back(std::move(p));
    }
}

HRESULT H264Decoder::Feed(const uint8_t* data, size_t size, int64_t timeHns)
{
    ComPtr<IMFSample> s;
    HRESULT hr = MakeMemorySample(data, size, timeHns, 333333, &s);
    if (FAILED(hr)) {
        return hr;
    }
    hr = m_mft->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        hr = PullAll();
        if (FAILED(hr)) {
            return hr;
        }
        hr = m_mft->ProcessInput(0, s.Get(), 0);
    }
    if (FAILED(hr)) {
        m_lastError = L"ProcessInput";
        return hr;
    }
    return PullAll();
}

HRESULT H264Decoder::Drain()
{
    HRESULT hr = m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(hr)) {
        return hr;
    }
    hr = m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        return hr;
    }
    return PullAll();
}

// ---------------------------------------------------------------- our transform, end to end

namespace {

// The class factory MFTRegisterLocal needs. Same object as the DLL's, built here so the test
// exercises the registration path and not a private back door.
class TestFactory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const long n = InterlockedDecrement(&m_ref);
        if (n == 0) {
            delete this;
        }
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        return Bc250CreateH264EncoderMFT(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }

private:
    long m_ref = 1;
};

// Finds our transform through MFTEnumEx, which is what a real client does.
HRESULT FindOurTransformByEnumeration(IMFTransform** out, bool* foundHardwareFlag)
{
    *out = nullptr;
    *foundHardwareFlag = false;
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                               MFT_ENUM_FLAG_LOCALMFT,
                           nullptr, &outInfo, &activates, &count);
    if (FAILED(hr)) {
        return hr;
    }
    HRESULT result = MF_E_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i) {
        GUID clsid = GUID_NULL;
        wchar_t* url = nullptr;
        UINT32 len = 0;
        bool ours = false;
        bool identified = false;
        if (SUCCEEDED(activates[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
            identified = true;
            ours = (clsid == CLSID_Bc250H264EncoderMFT);
        }
        if (!ours && SUCCEEDED(activates[i]->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute,
                                                                &url, &len))) {
            identified = true;
            ours = (wcscmp(url, L"amdgpu_wddm://h264-encoder/0") == 0);
            CoTaskMemFree(url);
        }
        // Anything the activation object already names is left alone. Creating another vendor's
        // hardware encoder just to look at it opens an encode session on that device, and doing so on
        // this host left the next process's sink writer blocked for minutes, so only entries that
        // expose neither a CLSID nor a hardware URL - which is what MFTRegisterLocal produces - are
        // instantiated for identification.
        if (identified && !ours) {
            activates[i]->Release();
            continue;
        }
        // MFTRegisterLocal takes an IClassFactory, not a CLSID, so the activation object it hands back
        // carries neither MFT_TRANSFORM_CLSID_Attribute nor the enumeration attributes the instance
        // sets in its constructor. The only way to recognise such an entry is to create it and ask the
        // instance; anything that is not ours is released again immediately.
        ComPtr<IMFTransform> candidate;
        if (!ours) {
            if (SUCCEEDED(activates[i]->ActivateObject(__uuidof(IMFTransform),
                                                       reinterpret_cast<void**>(&candidate)))) {
                ComPtr<IMFAttributes> ia;
                wchar_t* iurl = nullptr;
                UINT32 ilen = 0;
                if (SUCCEEDED(candidate->GetAttributes(&ia)) &&
                    SUCCEEDED(ia->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &iurl,
                                                     &ilen))) {
                    ours = (wcscmp(iurl, L"amdgpu_wddm://h264-encoder/0") == 0);
                    CoTaskMemFree(iurl);
                }
                if (!ours) {
                    candidate.Reset();
                    activates[i]->ShutdownObject();
                }
            }
        }
        if (ours && *out == nullptr) {
            UINT32 async = 0;
            if (FAILED(activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && candidate) {
                ComPtr<IMFAttributes> ia;
                if (SUCCEEDED(candidate->GetAttributes(&ia))) {
                    ia->GetUINT32(MF_TRANSFORM_ASYNC, &async);
                }
            }
            *foundHardwareFlag = (async != 0);
            if (candidate) {
                *out = candidate.Detach();
                result = S_OK;
            } else {
                result = activates[i]->ActivateObject(__uuidof(IMFTransform),
                                                     reinterpret_cast<void**>(out));
            }
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return result;
}

// What the inbox encoder does with CODECAPI_AVEncVideoEncodeQP, reported next to our own contract.
// The property is a UINT64 (codecapi.h:908) and is documented as carrying one quantiser per picture
// type in 16-bit fields, but no copy of that page is in the workspace reference set, so the inbox
// encoder on this machine is the reference this test can actually read. Diagnostic only: this prints
// what the inbox encoder answers and asserts nothing about it. Our own behaviour is asserted in
// RunMft, and the two have to agree on the variant type and on accepting the packed form.
void ProbeInboxQuantiserContract(const Options& o)
{
    ComPtr<IMFTransform> mft;
    HRESULT hr = CoCreateInstance(kInboxH264EncoderClsid, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMFTransform), reinterpret_cast<void**>(&mft));
    if (FAILED(hr)) {
        Say("inbox encoder not available for the quantiser probe: 0x%08lX",
            static_cast<unsigned long>(hr));
        return;
    }
    ComPtr<IMFMediaType> outType, inType;
    if (SUCCEEDED(MakeVideoType(MFVideoFormat_H264, o.width, o.height, o.fps, &outType))) {
        outType->SetUINT32(MF_MT_AVG_BITRATE, o.bitrate);
        mft->SetOutputType(0, outType.Get(), 0);
    }
    if (SUCCEEDED(MakeVideoType(MFVideoFormat_NV12, o.width, o.height, o.fps, &inType))) {
        mft->SetInputType(0, inType.Get(), 0);
    }
    ComPtr<ICodecAPI> codec;
    hr = mft->QueryInterface(__uuidof(ICodecAPI), reinterpret_cast<void**>(&codec));
    if (FAILED(hr)) {
        Say("the inbox encoder has no ICodecAPI: 0x%08lX", static_cast<unsigned long>(hr));
        return;
    }
    Say("inbox encoder, CODECAPI_AVEncVideoEncodeQP: IsSupported 0x%08lX",
        static_cast<unsigned long>(codec->IsSupported(&CODECAPI_AVEncVideoEncodeQP)));
    // Under CBR the quantiser belongs to the rate control, and an encoder is entitled to refuse to
    // be told one. The quality mode is where a per-picture quantiser means something, so the probe
    // asks for it first and reports whether that was accepted.
    {
        VARIANT v = {};
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = static_cast<ULONG>(eAVEncCommonRateControlMode_Quality);
        const HRESULT rc = codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.ulVal = 70;
        const HRESULT qh = codec->SetValue(&CODECAPI_AVEncCommonQuality, &v);
        VariantClear(&v);
        Say("  rate control to quality 0x%08lX, quality 70 0x%08lX",
            static_cast<unsigned long>(rc), static_cast<unsigned long>(qh));
    }
    // Zeroed, not only VariantInit'ed: these are printed as UINT64 whatever variant type comes back,
    // and a VT_UI4 answer leaves the high half of the union untouched. Reading those bits is the
    // mistake this probe is about, so the probe itself must not make it.
    VARIANT lo = {}, hi = {}, step = {};
    VariantInit(&lo); VariantInit(&hi); VariantInit(&step);
    const HRESULT rh = codec->GetParameterRange(&CODECAPI_AVEncVideoEncodeQP, &lo, &hi, &step);
    Say("  GetParameterRange 0x%08lX vt %u/%u/%u lo 0x%llX hi 0x%llX",
        static_cast<unsigned long>(rh), lo.vt, hi.vt, step.vt,
        static_cast<unsigned long long>(lo.ullVal), static_cast<unsigned long long>(hi.ullVal));
    VariantClear(&lo); VariantClear(&hi); VariantClear(&step);
    struct Probe { ULONGLONG value; VARTYPE vt; const char* what; };
    const Probe probes[] = {
        { 30ull, VT_UI8, "a plain scalar as VT_UI8" },
        { 30ull, VT_UI4, "a plain scalar as VT_UI4" },
        { 26ull | (28ull << 16) | (30ull << 32), VT_UI8, "the packed per-type form" },
        { 52ull, VT_UI8, "an I field above 51" },
        { 26ull | (99ull << 16), VT_UI8, "a P field above 51" },
    };
    for (const Probe& p : probes) {
        VARIANT v = {};
        VariantInit(&v);
        v.vt = p.vt;
        if (p.vt == VT_UI8) { v.ullVal = p.value; } else { v.ulVal = static_cast<ULONG>(p.value); }
        const HRESULT sh = codec->SetValue(&CODECAPI_AVEncVideoEncodeQP, &v);
        VariantClear(&v);
        VARIANT back = {};
        VariantInit(&back);
        const HRESULT gh = codec->GetValue(&CODECAPI_AVEncVideoEncodeQP, &back);
        Say("  SetValue %-28s 0x%08lX, GetValue 0x%08lX vt %u value 0x%llX", p.what,
            static_cast<unsigned long>(sh), static_cast<unsigned long>(gh), back.vt,
            static_cast<unsigned long long>(back.ullVal));
        VariantClear(&back);
    }
}

struct MftRun {
    uint32_t needInputEvents = 0;
    uint32_t haveOutputEvents = 0;
    uint32_t outputs = 0;
    uint32_t keyFrames = 0;
    uint64_t bytes = 0;
    bool drained = false;
    // How many pictures the transform asked for before it produced its first access unit. One is the
    // serial shape; more means it recorded a picture's GPU work while it still owed an output for an
    // earlier one, which is the only thing a client can see of the pipeline from the outside.
    uint32_t needBeforeFirstOutput = 0;
};

} // namespace

// DllCanUnloadNow must answer S_FALSE while a transform or a class object of ours is alive, and
// S_OK only once everything is released. A client reaches the transform through CoCreateInstance,
// for which COM never calls IClassFactory::LockServer, so without a per-object module lock the host
// would be free to unmap the DLL between two frames of a live recording.
int CheckModuleLock()
{
    int failures = 0;
    if (DllCanUnloadNow() != S_OK) {
        Say("FAIL DllCanUnloadNow does not start at S_OK");
        ++failures;
    }
    {
        ComPtr<IMFTransform> one;
        HRESULT hr = Bc250CreateH264EncoderMFT(__uuidof(IMFTransform),
                                               reinterpret_cast<void**>(&one));
        if (FAILED(hr) || !one) {
            SayHr("Bc250CreateH264EncoderMFT", hr);
            return failures + 1;
        }
        if (DllCanUnloadNow() != S_FALSE) {
            Say("FAIL DllCanUnloadNow says S_OK while a transform object is alive");
            ++failures;
        }
    }
    if (DllCanUnloadNow() != S_OK) {
        Say("FAIL DllCanUnloadNow still says S_FALSE after the transform was released");
        ++failures;
    }
    {
        // The class object counts too: a client may hold it across activations.
        ComPtr<IClassFactory> cf;
        HRESULT hr = DllGetClassObject(CLSID_Bc250H264EncoderMFT, __uuidof(IClassFactory),
                                       reinterpret_cast<void**>(&cf));
        if (FAILED(hr) || !cf) {
            SayHr("DllGetClassObject", hr);
            return failures + 1;
        }
        if (DllCanUnloadNow() != S_FALSE) {
            Say("FAIL DllCanUnloadNow says S_OK while a class object is alive");
            ++failures;
        }
        // And an object created through it, the route a real client takes.
        ComPtr<IMFTransform> two;
        hr = cf->CreateInstance(nullptr, __uuidof(IMFTransform),
                                reinterpret_cast<void**>(&two));
        if (FAILED(hr) || !two) {
            SayHr("IClassFactory::CreateInstance", hr);
            return failures + 1;
        }
        cf.Reset();
        if (DllCanUnloadNow() != S_FALSE) {
            Say("FAIL DllCanUnloadNow says S_OK while an activated transform is alive");
            ++failures;
        }
    }
    if (DllCanUnloadNow() != S_OK) {
        Say("FAIL DllCanUnloadNow does not return to S_OK");
        ++failures;
    }
    if (failures == 0) {
        Say("DllCanUnloadNow: S_FALSE while a transform or a class object lives, S_OK after");
    }
    return failures;
}

int RunMft(const Options& o)
{
    printf("mft: the asynchronous hardware transform contract, D3D11 BGRA input\n");

    const int moduleLockFailures = CheckModuleLock();

    ComPtr<IClassFactory> factory;
    factory.Attach(new (std::nothrow) TestFactory());
    if (!factory) {
        Say("out of memory");
        return 2;
    }
    HRESULT hr = RegisterLocal(factory.Get());
    if (FAILED(hr)) {
        SayHr("MFTRegisterLocal", hr);
        return 2;
    }
    // Which MFTEnumEx filter actually returns a locally registered hardware encoder is not obvious
    // from the headers, so report every combination before relying on one.
    {
        struct { const wchar_t* name; UINT32 flags; } probes[] = {
            { L"LOCALMFT|HARDWARE|ASYNCMFT",
              MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT },
            { L"LOCALMFT|ASYNCMFT", MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_ASYNCMFT },
            { L"LOCALMFT|HARDWARE", MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_HARDWARE },
            { L"LOCALMFT", MFT_ENUM_FLAG_LOCALMFT },
            { L"LOCALMFT|ALL", MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_ALL },
            { L"ALL", MFT_ENUM_FLAG_ALL },
        };
        MFT_REGISTER_TYPE_INFO probeOut = { MFMediaType_Video, MFVideoFormat_H264 };
        for (const auto& p : probes) {
            IMFActivate** list = nullptr;
            UINT32 n = 0;
            const HRESULT ph = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, p.flags, nullptr, &probeOut,
                                         &list, &n);
            UINT32 mine = 0;
            for (UINT32 k = 0; k < n; ++k) {
                GUID c = GUID_NULL;
                if (SUCCEEDED(list[k]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &c)) &&
                    c == CLSID_Bc250H264EncoderMFT) {
                    ++mine;
                }
                list[k]->Release();
            }
            CoTaskMemFree(list);
            printf("  MFTEnumEx %-26ls 0x%08lX, %u transform(s), ours by clsid %u\n", p.name,
                   static_cast<unsigned long>(ph), n, mine);
        }
    }
    ComPtr<IMFTransform> mft;
    bool asyncFlag = false;
    hr = FindOurTransformByEnumeration(&mft, &asyncFlag);
    if (FAILED(hr) || !mft) {
        SayHr("MFTEnumEx did not find the locally registered transform", hr);
        UnregisterLocal();
        return 1;
    }
    Say("found through MFTEnumEx with MFT_ENUM_FLAG_HARDWARE, MF_TRANSFORM_ASYNC %s",
        asyncFlag ? "set" : "NOT SET");

    // A client that has not unlocked the asynchronous model must be refused.
    ComPtr<IMFAttributes> attrs;
    hr = mft->GetAttributes(&attrs);
    if (FAILED(hr)) {
        SayHr("GetAttributes", hr);
        UnregisterLocal();
        return 2;
    }
    const HRESULT locked = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (locked != MF_E_TRANSFORM_ASYNC_LOCKED) {
        Say("FAIL a locked transform answered 0x%08lX, expected MF_E_TRANSFORM_ASYNC_LOCKED",
            static_cast<unsigned long>(locked));
        UnregisterLocal();
        return 1;
    }
    Say("streaming before MF_TRANSFORM_ASYNC_UNLOCK is refused, as the contract requires");
    hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 1);
    if (FAILED(hr)) {
        SayHr("SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK)", hr);
        UnregisterLocal();
        return 2;
    }

    // Our own Direct3D 11 device, handed over the way a capture pipeline hands over its device.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                           D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        SayHr("D3D11CreateDevice", hr);
        UnregisterLocal();
        return 2;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D10Multithread),
                                         reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
    }
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &manager);
    if (SUCCEEDED(hr)) {
        hr = manager->ResetDevice(device.Get(), token);
    }
    if (FAILED(hr)) {
        SayHr("MFCreateDXGIDeviceManager/ResetDevice", hr);
        UnregisterLocal();
        return 2;
    }
    hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                             reinterpret_cast<ULONG_PTR>(manager.Get()));
    if (FAILED(hr)) {
        SayHr("MFT_MESSAGE_SET_D3D_MANAGER", hr);
        UnregisterLocal();
        return 2;
    }

    // Negotiation has to refuse what the encoder cannot code exactly. 4:2:0 chroma lives on an even
    // grid, so an odd frame size - 1365x767 out of a window capture, say - has to come back as
    // MF_E_INVALIDMEDIATYPE while the client can still renegotiate, instead of being accepted and
    // then encoded one pixel smaller than MF_MT_FRAME_SIZE says.
    {
        uint32_t rejectFailures = 0;
        const UINT32 sizes[][2] = { { 639, 480 }, { 640, 479 }, { 1365, 767 } };
        for (const auto& s : sizes) {
            ComPtr<IMFMediaType> odd;
            if (FAILED(MakeVideoType(MFVideoFormat_H264, s[0], s[1], o.fps, &odd))) {
                continue;
            }
            HRESULT oh = mft->SetOutputType(0, odd.Get(), MFT_SET_TYPE_TEST_ONLY);
            if (oh != MF_E_INVALIDMEDIATYPE) {
                Say("FAIL SetOutputType(%ux%u) returned 0x%08lX, expected MF_E_INVALIDMEDIATYPE",
                    s[0], s[1], static_cast<unsigned long>(oh));
                ++rejectFailures;
            }
            ComPtr<IMFMediaType> oddIn;
            if (SUCCEEDED(MakeVideoType(MFVideoFormat_NV12, s[0], s[1], o.fps, &oddIn))) {
                oh = mft->SetInputType(0, oddIn.Get(), MFT_SET_TYPE_TEST_ONLY);
                if (oh != MF_E_INVALIDMEDIATYPE) {
                    Say("FAIL SetInputType(%ux%u) returned 0x%08lX, expected "
                        "MF_E_INVALIDMEDIATYPE", s[0], s[1], static_cast<unsigned long>(oh));
                    ++rejectFailures;
                }
            }
        }
        if (rejectFailures == 0) {
            Say("an odd frame size is refused on both types during negotiation");
        } else {
            UnregisterLocal();
            return 1;
        }
    }

    // Types: the output first, as an encoder requires, then the input.
    ComPtr<IMFMediaType> outType;
    hr = MakeVideoType(MFVideoFormat_H264, o.width, o.height, o.fps, &outType);
    if (SUCCEEDED(hr)) { hr = outType->SetUINT32(MF_MT_AVG_BITRATE, o.bitrate); }
    if (SUCCEEDED(hr)) { hr = outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, o.gop); }
    if (SUCCEEDED(hr)) { hr = mft->SetOutputType(0, outType.Get(), 0); }
    if (FAILED(hr)) {
        SayHr("SetOutputType(H264)", hr);
        UnregisterLocal();
        return 2;
    }
    ComPtr<IMFMediaType> current;
    hr = mft->GetOutputCurrentType(0, &current);
    if (SUCCEEDED(hr)) {
        UINT32 blobSize = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blobSize)) && blobSize > 0) {
            Say("MF_MT_MPEG_SEQUENCE_HEADER present on the output type, %u bytes", blobSize);
        } else {
            Say("FAIL the output type carries no MF_MT_MPEG_SEQUENCE_HEADER");
            UnregisterLocal();
            return 1;
        }
    }
    ComPtr<IMFMediaType> inType;
    hr = MakeVideoType(MFVideoFormat_ARGB32, o.width, o.height, o.fps, &inType);
    if (SUCCEEDED(hr)) { hr = mft->SetInputType(0, inType.Get(), 0); }
    if (FAILED(hr)) {
        SayHr("SetInputType(ARGB32)", hr);
        UnregisterLocal();
        return 2;
    }

    // Counts every contract failure that is not fatal on its own; the run's exit code folds it in.
    uint32_t settingFailures = 0;

    // The colour description has to come from the input type. Windows Camera hands over NV12 tagged
    // BT.601; nothing in this encoder converts, so the VUI has to repeat what the client said. The
    // one exception is the BGRA path, where cs_import.hlsl itself converts with the studio-range
    // BT.709 matrix, so the VUI says 709 whatever the RGB type claims.
    //
    // The oracle is exact, not comparative: for each input type the expected sequence header is
    // built here from the encoder's own writer, with the configuration SetOutputType derived (size,
    // frame rate, bitrate) and the colour description the transform should have read, and the bytes
    // on the output type have to be those bytes. A check that "different inputs give different
    // headers" would also pass if the headers differed in the wrong field.
    {
        auto blobOf = [&mft](std::vector<uint8_t>* out) -> bool {
            out->clear();
            ComPtr<IMFMediaType> t;
            if (FAILED(mft->GetOutputCurrentType(0, &t))) {
                return false;
            }
            UINT32 size = 0;
            if (FAILED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) {
                return false;
            }
            out->resize(size);
            return SUCCEEDED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, out->data(), size, &size));
        };
        auto expectedBlob = [&o](uint32_t primaries, uint32_t transfer, uint32_t matrix,
                                 bool full) -> std::vector<uint8_t> {
            EncoderConfig want;
            want.width = o.width;
            want.height = o.height;
            want.fpsNum = o.fps;
            want.fpsDen = 1;
            want.meanBitRate = o.bitrate;
            want.gopSize = o.gop;
            want.colourPrimaries = primaries;
            want.transferCharacteristics = transfer;
            want.matrixCoefficients = matrix;
            want.fullRange = full;
            SequenceParams sps;
            PictureParams pps;
            MakeSequenceParams(want, &sps, &pps);
            std::vector<uint8_t> nals;
            BuildParameterSetNals(nals, sps, pps);
            return nals;
        };
        // Every tagged case carries MFVideoTransFunc_709, so that the cases differ only in what
        // their tag is about. The untagged case is the one that has to come out "unspecified" (2).
        struct ColourCase {
            const GUID* subtype;
            bool tagged;
            UINT32 primaries, transfer, matrix, range;
            uint32_t wantPrimaries, wantTransfer, wantMatrix;
            bool wantFull;
            const char* what;
        };
        const ColourCase cases[] = {
            { &MFVideoFormat_NV12, true, MFVideoPrimaries_SMPTE170M, MFVideoTransFunc_709,
              MFVideoTransferMatrix_BT601, MFNominalRange_16_235, 6, 1, 6, false,
              "NV12 tagged BT.601, the shape Windows Camera delivers" },
            { &MFVideoFormat_NV12, true, MFVideoPrimaries_BT709, MFVideoTransFunc_709,
              MFVideoTransferMatrix_BT709, MFNominalRange_16_235, 1, 1, 1, false,
              "NV12 tagged BT.709 studio range" },
            { &MFVideoFormat_NV12, true, MFVideoPrimaries_BT709, MFVideoTransFunc_709,
              MFVideoTransferMatrix_BT709, MFNominalRange_0_255, 1, 1, 1, true,
              "NV12 tagged BT.709 full range" },
            { &MFVideoFormat_NV12, false, 0, 0, 0, 0, 2, 2, 2, false,
              "NV12 with no colour description, which is unspecified and not a guess" },
            { &MFVideoFormat_ARGB32, true, MFVideoPrimaries_SMPTE170M, MFVideoTransFunc_240M,
              MFVideoTransferMatrix_BT601, MFNominalRange_0_255, 1, 1, 1, false,
              "ARGB32 tagged BT.601 full range, which the import shader converts to 709 studio" },
        };
        uint32_t colourFailures = 0;
        std::vector<uint8_t> bt601Header;
        for (const ColourCase& c : cases) {
            ComPtr<IMFMediaType> t;
            if (FAILED(MakeVideoType(*c.subtype, o.width, o.height, o.fps, &t))) {
                ++colourFailures;
                continue;
            }
            if (c.tagged) {
                t->SetUINT32(MF_MT_VIDEO_PRIMARIES, c.primaries);
                t->SetUINT32(MF_MT_TRANSFER_FUNCTION, c.transfer);
                t->SetUINT32(MF_MT_YUV_MATRIX, c.matrix);
                t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, c.range);
            }
            const HRESULT sh = mft->SetInputType(0, t.Get(), 0);
            std::vector<uint8_t> got;
            if (FAILED(sh) || !blobOf(&got)) {
                Say("FAIL %s: SetInputType 0x%08lX, %zu blob bytes", c.what,
                    static_cast<unsigned long>(sh), got.size());
                ++colourFailures;
                continue;
            }
            const std::vector<uint8_t> want =
                expectedBlob(c.wantPrimaries, c.wantTransfer, c.wantMatrix, c.wantFull);
            if (got != want) {
                Say("FAIL %s: the sequence header is not the one for colour %u/%u/%u %s range "
                    "(%zu bytes against %zu)", c.what, c.wantPrimaries, c.wantTransfer,
                    c.wantMatrix, c.wantFull ? "full" : "studio", got.size(), want.size());
                ++colourFailures;
                continue;
            }
            if (bt601Header.empty()) {
                bt601Header = got;
            } else if (got == bt601Header) {
                // Every later case describes different colour, so it cannot repeat the first header.
                Say("FAIL %s gives the same sequence header as the BT.601 case", c.what);
                ++colourFailures;
            }
        }
        // Back to the untagged ARGB32 type the rest of this run encodes from.
        if (FAILED(mft->SetInputType(0, inType.Get(), 0))) {
            Say("FAIL the input type could not be set back to ARGB32");
            ++colourFailures;
        }
        if (colourFailures != 0) {
            settingFailures += colourFailures;
        } else {
            Say("the VUI is the client's own colour description, byte for byte: 5 input types, "
                "BT.601, BT.709, full range, unspecified, and 709 for the converting BGRA path");
        }
    }

    // The settings Game Bar and Chromium write.
    ComPtr<ICodecAPI> codec;
    hr = mft->QueryInterface(__uuidof(ICodecAPI), reinterpret_cast<void**>(&codec));
    if (FAILED(hr)) {
        SayHr("QueryInterface(ICodecAPI)", hr);
        UnregisterLocal();
        return 2;
    }
    // The type matters: AVEncCommonLowLatency is a VT_BOOL property, so a conformant encoder returns
    // VARIANT_TRUE (-1) from GetValue, not 1. Clients set it either way, so the setter takes VT_UI4 as
    // well, and the check below sets each property in its documented type and compares in that type.
    struct Setting { const GUID* id; const char* name; VARTYPE vt; ULONG value; };
    const Setting settings[] = {
        { &CODECAPI_AVEncCommonRateControlMode, "AVEncCommonRateControlMode", VT_UI4,
          static_cast<ULONG>(eAVEncCommonRateControlMode_CBR) },
        { &CODECAPI_AVEncCommonMeanBitRate, "AVEncCommonMeanBitRate", VT_UI4, o.bitrate },
        { &CODECAPI_AVEncMPVGOPSize, "AVEncMPVGOPSize", VT_UI4, o.gop },
        { &CODECAPI_AVEncCommonLowLatency, "AVEncCommonLowLatency", VT_BOOL, 1 },
    };
    for (const Setting& s : settings) {
        VARIANT v;
        VariantInit(&v);
        v.vt = s.vt;
        if (s.vt == VT_BOOL) {
            v.boolVal = (s.value != 0) ? VARIANT_TRUE : VARIANT_FALSE;
        } else {
            v.ulVal = s.value;
        }
        HRESULT sh = codec->IsSupported(s.id);
        if (sh != S_OK) {
            Say("FAIL ICodecAPI::IsSupported(%s) returned 0x%08lX", s.name,
                static_cast<unsigned long>(sh));
            ++settingFailures;
        }
        sh = codec->SetValue(s.id, &v);
        if (FAILED(sh)) {
            Say("FAIL ICodecAPI::SetValue(%s) returned 0x%08lX", s.name,
                static_cast<unsigned long>(sh));
            ++settingFailures;
        }
        VARIANT back;
        VariantInit(&back);
        sh = codec->GetValue(s.id, &back);
        const bool roundTripped =
            SUCCEEDED(sh) && back.vt == s.vt &&
            ((s.vt == VT_BOOL) ? (back.boolVal == v.boolVal) : (back.ulVal == s.value));
        if (!roundTripped) {
            Say("FAIL ICodecAPI::GetValue(%s) returned 0x%08lX vt %u value %ld, expected vt %u "
                "value %lu", s.name, static_cast<unsigned long>(sh), back.vt,
                static_cast<long>(back.lVal), s.vt, static_cast<unsigned long>(s.value));
            ++settingFailures;
        }
        VariantClear(&back);
        // A client that sets a boolean property as VT_UI4, which several do, must still be accepted.
        if (s.vt == VT_BOOL) {
            VARIANT alt;
            VariantInit(&alt);
            alt.vt = VT_UI4;
            alt.ulVal = s.value;
            const HRESULT ah = codec->SetValue(s.id, &alt);
            if (FAILED(ah)) {
                Say("FAIL ICodecAPI::SetValue(%s) as VT_UI4 returned 0x%08lX", s.name,
                    static_cast<unsigned long>(ah));
                ++settingFailures;
            }
            VariantClear(&alt);
        }
        VariantClear(&v);
    }
    // Low latency back off, which is where the default is and where the streaming part of this case
    // runs. The setting is not decoration: it is what the transform's pipeline depth follows, so the
    // loop below feeds a transform that records one picture's GPU work while it is still coding the
    // slice of the one before it, and the drain has a picture to flush. The serial shape is what the
    // same transform does with low latency on, and the sweep covers that side.
    {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_FALSE;
        HRESULT sh = codec->SetValue(&CODECAPI_AVEncCommonLowLatency, &v);
        VARIANT back;
        VariantInit(&back);
        if (SUCCEEDED(sh)) {
            sh = codec->GetValue(&CODECAPI_AVEncCommonLowLatency, &back);
        }
        if (FAILED(sh) || back.vt != VT_BOOL || back.boolVal != VARIANT_FALSE) {
            Say("FAIL clearing AVEncCommonLowLatency returned 0x%08lX vt %u value %ld",
                static_cast<unsigned long>(sh), back.vt, static_cast<long>(back.lVal));
            ++settingFailures;
        }
        VariantClear(&back);
        VariantClear(&v);
    }
    // CABAC has to be refused honestly, not silently accepted.
    {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        const HRESULT sh = codec->SetValue(&CODECAPI_AVEncH264CABACEnable, &v);
        if (sh != E_INVALIDARG) {
            Say("FAIL enabling CABAC returned 0x%08lX, expected E_INVALIDARG",
                static_cast<unsigned long>(sh));
            ++settingFailures;
        }
        VariantClear(&v);
    }
    // What the inbox encoder does with the same property, printed for comparison before we assert
    // our own behaviour.
    ProbeInboxQuantiserContract(o);
    // The quantiser through ICodecAPI. CODECAPI_AVEncVideoEncodeQP is a UINT64 (codecapi.h:908)
    // carrying 16 bits per picture type (I at bit 0, P at 16, B at 32), which is what the probe above
    // shows the inbox encoder doing. So both the packed form and a plain scalar have to work, an
    // out-of-range field has to be refused, and the answer has to come back as VT_UI8. Min and max
    // quantiser are UINT32.
    {
        VARIANT lo, hi, step;
        VariantInit(&lo); VariantInit(&hi); VariantInit(&step);
        if (FAILED(codec->GetParameterRange(&CODECAPI_AVEncVideoEncodeQP, &lo, &hi, &step)) ||
            lo.vt != VT_UI8 || hi.vt != VT_UI8 || (lo.ullVal & 0xFFFFull) == 0) {
            Say("FAIL GetParameterRange(AVEncVideoEncodeQP) vt %u/%u lo %llu", lo.vt, hi.vt,
                static_cast<unsigned long long>(lo.ullVal));
            ++settingFailures;
        }
        const ULONGLONG advertisedFloor = lo.ullVal & 0xFFFFull;
        VariantClear(&lo); VariantClear(&hi); VariantClear(&step);

        struct QpCase { ULONGLONG set; VARTYPE vt; HRESULT want; ULONG readBack; const char* what; };
        const QpCase qpCases[] = {
            { 32ull, VT_UI4, S_OK, 32, "a plain scalar in the smallest variant type" },
            { 26ull | (28ull << 16) | (30ull << 32), VT_UI8, S_OK, 26, "the packed per-type form" },
            { 52ull, VT_UI8, E_INVALIDARG, 0, "an I field above 51" },
            { 26ull | (99ull << 16), VT_UI8, E_INVALIDARG, 0, "a P field above 51" },
            { advertisedFloor | (advertisedFloor << 16) | (advertisedFloor << 32), VT_UI8, S_OK,
              static_cast<ULONG>(advertisedFloor), "the advertised floor" },
        };
        for (const QpCase& q : qpCases) {
            VARIANT v;
            VariantInit(&v);
            v.vt = q.vt;
            if (q.vt == VT_UI8) { v.ullVal = q.set; } else { v.ulVal = static_cast<ULONG>(q.set); }
            const HRESULT sh = codec->SetValue(&CODECAPI_AVEncVideoEncodeQP, &v);
            VariantClear(&v);
            if (sh != q.want) {
                Say("FAIL SetValue(AVEncVideoEncodeQP) with %s returned 0x%08lX, expected 0x%08lX",
                    q.what, static_cast<unsigned long>(sh), static_cast<unsigned long>(q.want));
                ++settingFailures;
                continue;
            }
            if (FAILED(q.want)) {
                continue;
            }
            VARIANT back;
            VariantInit(&back);
            const HRESULT gh = codec->GetValue(&CODECAPI_AVEncVideoEncodeQP, &back);
            const bool ok = SUCCEEDED(gh) && back.vt == VT_UI8 &&
                            (back.ullVal & 0xFFFFull) == q.readBack &&
                            ((back.ullVal >> 16) & 0xFFFFull) == q.readBack;
            if (!ok) {
                Say("FAIL GetValue(AVEncVideoEncodeQP) after %s: 0x%08lX vt %u value %llu", q.what,
                    static_cast<unsigned long>(gh), back.vt,
                    static_cast<unsigned long long>(back.ullVal));
                ++settingFailures;
            }
            VariantClear(&back);
        }
        for (const GUID* id : { &CODECAPI_AVEncVideoMinQP, &CODECAPI_AVEncVideoMaxQP }) {
            const bool isMin = (id == &CODECAPI_AVEncVideoMinQP);
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = isMin ? 20u : 44u;
            const HRESULT sh = codec->SetValue(id, &v);
            VARIANT back;
            VariantInit(&back);
            const HRESULT gh = codec->GetValue(id, &back);
            if (FAILED(sh) || FAILED(gh) || back.vt != VT_UI4 || back.ulVal != v.ulVal) {
                Say("FAIL the quantiser range round trip: set 0x%08lX get 0x%08lX vt %u value %lu",
                    static_cast<unsigned long>(sh), static_cast<unsigned long>(gh), back.vt,
                    static_cast<unsigned long>(back.ulVal));
                ++settingFailures;
            }
            VariantClear(&back);
            // A bound below the floor the encoder will use: accepted, because the request is legal,
            // and read back as the floor, because that is the bound that will apply. The floor is
            // the one GetParameterRange advertises for this property.
            VARIANT boundLo = {}, boundHi = {}, boundStep = {};
            VariantInit(&boundLo); VariantInit(&boundHi); VariantInit(&boundStep);
            if (SUCCEEDED(codec->GetParameterRange(id, &boundLo, &boundHi, &boundStep)) &&
                boundLo.vt == VT_UI4) {
                VARIANT zero = {};
                VariantInit(&zero);
                zero.vt = VT_UI4;
                zero.ulVal = 0;
                const HRESULT zh = codec->SetValue(id, &zero);
                VariantClear(&zero);
                VARIANT readBack = {};
                VariantInit(&readBack);
                const HRESULT rh2 = codec->GetValue(id, &readBack);
                if (FAILED(zh) || FAILED(rh2) || readBack.vt != VT_UI4 ||
                    readBack.ulVal != boundLo.ulVal) {
                    Say("FAIL a quantiser bound of 0 did not come back as the advertised floor %lu: "
                        "set 0x%08lX get 0x%08lX vt %u value %lu",
                        static_cast<unsigned long>(boundLo.ulVal), static_cast<unsigned long>(zh),
                        static_cast<unsigned long>(rh2), readBack.vt,
                        static_cast<unsigned long>(readBack.ulVal));
                    ++settingFailures;
                }
                VariantClear(&readBack);
            }
            VariantClear(&boundLo); VariantClear(&boundHi); VariantClear(&boundStep);
            // Back to the default bound, so that this probe does not clamp the run's own quantiser.
            v.ulVal = isMin ? 14u : 46u;
            codec->SetValue(id, &v);
            VariantClear(&v);
        }
        // Back to the quantiser this run is supposed to encode at, in the packed form.
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI8;
        v.ullVal = static_cast<ULONGLONG>(o.qp) | (static_cast<ULONGLONG>(o.qp) << 16) |
                   (static_cast<ULONGLONG>(o.qp) << 32);
        if (FAILED(codec->SetValue(&CODECAPI_AVEncVideoEncodeQP, &v))) {
            Say("FAIL SetValue(AVEncVideoEncodeQP) for the run's own quantiser");
            ++settingFailures;
        }
        VariantClear(&v);
    }
    if (settingFailures == 0) {
        Say("ICodecAPI: rate control, bitrate, GOP, low latency and the packed UINT64 quantiser set "
            "and read back; an out-of-range quantiser field and CABAC refused");
    }

    Pattern pattern;
    hr = pattern.Initialize(device.Get(), o.width, o.height);
    if (FAILED(hr)) {
        SayHr("Pattern::Initialize", hr);
        UnregisterLocal();
        return 2;
    }

    ComPtr<IMFMediaEventGenerator> events;
    hr = mft->QueryInterface(__uuidof(IMFMediaEventGenerator), reinterpret_cast<void**>(&events));
    if (FAILED(hr)) {
        SayHr("QueryInterface(IMFMediaEventGenerator)", hr);
        UnregisterLocal();
        return 2;
    }

    // The sequence header as it stands after every ICodecAPI setting, which is the order a capture
    // client uses: SetOutputType, then the settings, then streaming. A file sink copies these bytes
    // into the container, so they have to be the same bytes the encoder puts in band in front of the
    // first key frame; the comparison is at the end of this run.
    std::vector<uint8_t> blob;
    // The declared level that goes with those bytes, kept for the mid-stream check at the end.
    UINT32 levelWhileNegotiating = 0;
    {
        ComPtr<IMFMediaType> afterSettings;
        if (SUCCEEDED(mft->GetOutputCurrentType(0, &afterSettings))) {
            UINT32 size = 0;
            if (SUCCEEDED(afterSettings->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) &&
                size > 0) {
                blob.resize(size);
                if (FAILED(afterSettings->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), size,
                                                  &size))) {
                    blob.clear();
                }
            }
            afterSettings->GetUINT32(MF_MT_MPEG2_LEVEL, &levelWhileNegotiating);
        }
        if (blob.empty()) {
            Say("FAIL no MF_MT_MPEG_SEQUENCE_HEADER after the ICodecAPI settings");
            ++settingFailures;
        }
    }

    hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) { hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0); }
    if (FAILED(hr)) {
        SayHr("begin streaming", hr);
        UnregisterLocal();
        return 2;
    }

    // Which GPU the transform encodes on, which a client asks for by reading MFT_ENUM_ADAPTER_LUID
    // (mfapi.h:2025) off IMFTransform::GetAttributes. The attribute's documented data type is LUID,
    // so it has to be an 8 byte blob - a UINT64 of the same bits answers MF_E_INVALIDTYPE to the
    // GetBlob a client writes - and it has to name the adapter of the device the client handed over
    // through MFT_MESSAGE_SET_D3D_MANAGER, not an adapter the transform picked for itself.
    {
        LUID want = {};
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc = {};
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                             reinterpret_cast<void**>(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
            want = adapterDesc.AdapterLuid;
        }
        MF_ATTRIBUTE_TYPE luidType = MF_ATTRIBUTE_UINT32;
        LUID got = {};
        UINT32 luidSize = 0;
        if (FAILED(attrs->GetItemType(MFT_ENUM_ADAPTER_LUID, &luidType)) ||
            luidType != MF_ATTRIBUTE_BLOB) {
            Say("FAIL MFT_ENUM_ADAPTER_LUID is type %u on the transform's attribute store, not the "
                "documented LUID blob", static_cast<unsigned>(luidType));
            ++settingFailures;
        } else if (FAILED(attrs->GetBlobSize(MFT_ENUM_ADAPTER_LUID, &luidSize)) ||
                   luidSize != sizeof(got) ||
                   FAILED(attrs->GetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<UINT8*>(&got),
                                         sizeof(got), nullptr))) {
            Say("FAIL MFT_ENUM_ADAPTER_LUID is %u bytes, not a LUID", luidSize);
            ++settingFailures;
        } else if (got.LowPart != want.LowPart || got.HighPart != want.HighPart) {
            Say("FAIL MFT_ENUM_ADAPTER_LUID is %08lx:%08lx, the client's device is on %08lx:%08lx",
                static_cast<unsigned long>(got.HighPart), static_cast<unsigned long>(got.LowPart),
                static_cast<unsigned long>(want.HighPart),
                static_cast<unsigned long>(want.LowPart));
            ++settingFailures;
        } else {
            Say("MFT_ENUM_ADAPTER_LUID is the client's own adapter, %08lx:%08lx, as an 8 byte blob",
                static_cast<unsigned long>(got.HighPart), static_cast<unsigned long>(got.LowPart));
        }
    }

    H264Decoder dec;
    hr = dec.Initialize(o.width, o.height);
    if (FAILED(hr)) {
        SayHr("the inbox decoder MFT", hr);
        UnregisterLocal();
        return 2;
    }

    MftRun run;
    uint32_t fed = 0;
    bool endOfStream = false;
    const int64_t duration = 10000000LL / o.fps;
    const double t0 = NowMs();
    std::vector<uint8_t> stream;
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        hr = events->GetEvent(0, &ev);
        if (FAILED(hr)) {
            SayHr("GetEvent", hr);
            break;
        }
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            ++run.needInputEvents;
            if (fed < o.frames) {
                hr = pattern.Draw(fed);
                if (FAILED(hr)) {
                    SayHr("Pattern::Draw", hr);
                    break;
                }
                ComPtr<IMFMediaBuffer> buffer;
                hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pattern.Texture(), 0,
                                               FALSE, &buffer);
                if (FAILED(hr)) {
                    SayHr("MFCreateDXGISurfaceBuffer", hr);
                    break;
                }
                ComPtr<IMFSample> sample;
                hr = MFCreateSample(&sample);
                if (SUCCEEDED(hr)) { hr = sample->AddBuffer(buffer.Get()); }
                if (SUCCEEDED(hr)) {
                    hr = sample->SetSampleTime(static_cast<int64_t>(fed) * duration);
                }
                if (SUCCEEDED(hr)) { hr = sample->SetSampleDuration(duration); }
                if (SUCCEEDED(hr)) { hr = mft->ProcessInput(0, sample.Get(), 0); }
                if (FAILED(hr)) {
                    SayHr("ProcessInput", hr);
                    break;
                }
                ++fed;
            } else if (!endOfStream) {
                endOfStream = true;
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            }
        } else if (type == METransformHaveOutput) {
            ++run.haveOutputEvents;
            if (run.haveOutputEvents == 1) {
                run.needBeforeFirstOutput = run.needInputEvents;
            }
            MFT_OUTPUT_DATA_BUFFER buf = {};
            DWORD status = 0;
            hr = mft->ProcessOutput(0, 1, &buf, &status);
            if (FAILED(hr)) {
                SayHr("ProcessOutput", hr);
                break;
            }
            ComPtr<IMFSample> out;
            out.Attach(buf.pSample);
            if (buf.pEvents != nullptr) {
                buf.pEvents->Release();
            }
            ComPtr<IMFMediaBuffer> b;
            hr = out->ConvertToContiguousBuffer(&b);
            if (FAILED(hr)) {
                SayHr("ConvertToContiguousBuffer", hr);
                break;
            }
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            hr = b->Lock(&data, &maxLen, &curLen);
            if (FAILED(hr)) {
                SayHr("Lock", hr);
                break;
            }
            run.bytes += curLen;
            ++run.outputs;
            UINT32 clean = 0;
            if (SUCCEEDED(out->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean != 0) {
                ++run.keyFrames;
            }
            // mfapi.h:1166 declares MFSampleExtension_VideoEncodeQP as UINT64, so GetUINT64 is what
            // a client calls and it has to succeed.
            UINT64 sampleQp = 0;
            if (FAILED(out->GetUINT64(MFSampleExtension_VideoEncodeQP, &sampleQp)) ||
                sampleQp > 51) {
                Say("FAIL output %u carries no UINT64 MFSampleExtension_VideoEncodeQP",
                    run.outputs - 1);
                ++settingFailures;
            }
            if (run.outputs == 1 && !blob.empty()) {
                if (curLen < blob.size() || memcmp(data, blob.data(), blob.size()) != 0) {
                    Say("FAIL the first access unit does not open with the %zu bytes of "
                        "MF_MT_MPEG_SEQUENCE_HEADER: the container's parameter sets and the in-band "
                        "ones disagree", blob.size());
                    ++settingFailures;
                } else {
                    Say("the first access unit opens with the exact %zu bytes of "
                        "MF_MT_MPEG_SEQUENCE_HEADER", blob.size());
                }
            }
            stream.insert(stream.end(), data, data + curLen);
            const HRESULT fh = dec.Feed(data, curLen, static_cast<int64_t>(run.outputs - 1) * duration);
            b->Unlock();
            if (FAILED(fh)) {
                Say("FAIL the decoder rejected output %u: 0x%08lX %ls", run.outputs - 1,
                    static_cast<unsigned long>(fh), dec.LastError().c_str());
                break;
            }
            if (endOfStream && run.outputs >= o.frames) {
                // Nothing is buffered inside the transform, so the drain event may already be queued
                // behind this one; keep reading until it arrives.
                continue;
            }
        } else if (type == METransformDrainComplete) {
            run.drained = true;
            break;
        } else if (type == MEError) {
            HRESULT st = S_OK;
            ev->GetStatus(&st);
            Say("FAIL MEError 0x%08lX", static_cast<unsigned long>(st));
            break;
        }
    }
    const double t1 = NowMs();

    // A second segment after the drain, without NOTIFY_END_STREAMING in between. That is the
    // documented way to restart a stream, and it is what pause and resume in a recording client
    // does. The segment's first access unit has to be a key frame carrying its own parameter sets:
    // a P slice here would reference the previous segment's reconstruction and the new file would be
    // undecodable from its own start.
    if (run.drained) {
        bool restartFed = false, restartKey = false, restartParams = false, restartOut = false;
        bool restartDrained = false;
        HRESULT rh = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        while (SUCCEEDED(rh) && !restartOut) {
            ComPtr<IMFMediaEvent> ev;
            rh = events->GetEvent(0, &ev);
            if (FAILED(rh)) {
                break;
            }
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            // One picture, then a drain, because one picture need not produce an access unit: the
            // transform holds a picture back unless the client asked for low latency, and a client
            // that wants the one it fed has to close the segment. Which is what a client does anyway,
            // and waiting for an output instead of draining is how this loop used to hang.
            if (type == METransformNeedInput && restartFed && !restartDrained) {
                restartDrained = true;
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                rh = mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            } else if (type == METransformNeedInput && !restartFed) {
                ComPtr<IMFMediaBuffer> buffer;
                ComPtr<IMFSample> sample;
                rh = pattern.Draw(0);
                if (SUCCEEDED(rh)) {
                    rh = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pattern.Texture(), 0,
                                                   FALSE, &buffer);
                }
                if (SUCCEEDED(rh)) { rh = MFCreateSample(&sample); }
                if (SUCCEEDED(rh)) { rh = sample->AddBuffer(buffer.Get()); }
                if (SUCCEEDED(rh)) {
                    rh = sample->SetSampleTime(static_cast<int64_t>(fed) * duration);
                }
                if (SUCCEEDED(rh)) { rh = sample->SetSampleDuration(duration); }
                if (SUCCEEDED(rh)) { rh = mft->ProcessInput(0, sample.Get(), 0); }
                restartFed = SUCCEEDED(rh);
            } else if (type == METransformHaveOutput) {
                MFT_OUTPUT_DATA_BUFFER buf = {};
                DWORD status = 0;
                rh = mft->ProcessOutput(0, 1, &buf, &status);
                if (FAILED(rh)) {
                    break;
                }
                ComPtr<IMFSample> out;
                out.Attach(buf.pSample);
                if (buf.pEvents != nullptr) {
                    buf.pEvents->Release();
                }
                UINT32 clean = 0;
                restartKey = SUCCEEDED(out->GetUINT32(MFSampleExtension_CleanPoint, &clean)) &&
                             clean != 0;
                ComPtr<IMFMediaBuffer> b;
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                if (SUCCEEDED(out->ConvertToContiguousBuffer(&b)) &&
                    SUCCEEDED(b->Lock(&data, &maxLen, &curLen))) {
                    restartParams = !blob.empty() && curLen >= blob.size() &&
                                    memcmp(data, blob.data(), blob.size()) == 0;
                    b->Unlock();
                }
                restartOut = true;
            } else if (type == METransformDrainComplete) {
                // The drain cannot have left an access unit behind: its METransformHaveOutput is
                // queued before it. Reaching this with no output means the segment produced none, and
                // the check below says so rather than this loop waiting for one that will not come.
                break;
            } else if (type == MEError) {
                break;
            }
        }
        if (!restartFed || !restartOut || !restartKey || !restartParams) {
            Say("FAIL the segment after the drain: input accepted %d, output %d, key frame %d, "
                "parameter sets %d", restartFed ? 1 : 0, restartOut ? 1 : 0, restartKey ? 1 : 0,
                restartParams ? 1 : 0);
            ++settingFailures;
        } else {
            Say("after COMMAND_DRAIN, NOTIFY_START_OF_STREAM opens a new segment with a key frame "
                "carrying its own parameter sets");
        }
    }

    // A setting that would change the parameter sets, arriving while the encoder is already running.
    // A bitrate of 60 Mbit/s needs a higher level than the sequence header the sink has already
    // copied into the container, and a running encoder cannot change the parameter sets of a
    // sequence that is being decoded against them. So the blob has to stay exactly what the stream
    // carries, and the level on the output type with it: the honest answer, and the opposite of the
    // stale blob this transform used to publish when a setting changed.
    {
        VARIANT v = {};
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 60000000u;
        const HRESULT sh = codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        VariantClear(&v);
        std::vector<uint8_t> after;
        UINT32 levelAfter = 0;
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft->GetOutputCurrentType(0, &t))) {
            UINT32 size = 0;
            if (SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size != 0) {
                after.resize(size);
                t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, after.data(), size, &size);
            }
            t->GetUINT32(MF_MT_MPEG2_LEVEL, &levelAfter);
        }
        if (FAILED(sh) || after.empty() || after != blob || levelAfter != levelWhileNegotiating) {
            Say("FAIL a bitrate change while streaming moved the published parameter sets: "
                "SetValue 0x%08lX, %zu bytes against %zu, level %u against %u",
                static_cast<unsigned long>(sh), after.size(), blob.size(), levelAfter,
                levelWhileNegotiating);
            ++settingFailures;
        } else {
            Say("a bitrate change while streaming leaves the published parameter sets and the "
                "declared level exactly as the stream carries them");
        }
    }

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    dec.Drain();

    ComPtr<IMFShutdown> shutdown;
    if (SUCCEEDED(mft->QueryInterface(__uuidof(IMFShutdown), reinterpret_cast<void**>(&shutdown)))) {
        shutdown->Shutdown();
        MFSHUTDOWN_STATUS st = MFSHUTDOWN_INITIATED;
        if (FAILED(shutdown->GetShutdownStatus(&st)) || st != MFSHUTDOWN_COMPLETED) {
            Say("FAIL IMFShutdown did not report completion");
            ++settingFailures;
        }
        // Everything must refuse after shutdown.
        if (mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0) != MF_E_SHUTDOWN) {
            Say("FAIL the transform still accepts messages after Shutdown");
            ++settingFailures;
        }
    }
    mft.Reset();
    UnregisterLocal();

    Say("fed %u pictures, %u NeedInput, %u HaveOutput, %u outputs, %u key frames, drain %s",
        fed, run.needInputEvents, run.haveOutputEvents, run.outputs, run.keyFrames,
        run.drained ? "complete" : "NOT SIGNALLED");
    Say("the transform asked for %u pictures before its first access unit, so it %s",
        run.needBeforeFirstOutput,
        (run.needBeforeFirstOutput > 1) ? "pipelines" : "codes one picture at a time");
    Say("%llu bytes, %.0f bit/s, %.2f ms per picture through the transform interface",
        static_cast<unsigned long long>(run.bytes),
        (run.bytes * 8.0 * o.fps) / (run.outputs ? run.outputs : 1),
        (t1 - t0) / (run.outputs ? run.outputs : 1));

    // The decoder's own view: every access unit has to decode, and the result has to resemble the
    // drawn source. The draw is in BGR and the import pass converts, so this is a PSNR check against
    // the decoded picture of the first frame rather than a bit-exactness check.
    std::vector<Picture>& pics = dec.Pictures();
    Say("the inbox decoder accepted %zu of %u access units", pics.size(), run.outputs);

    int rc = 0;
    if (run.outputs != o.frames) {
        Say("FAIL %u outputs for %u inputs", run.outputs, o.frames);
        rc = 1;
    }
    if (!run.drained) {
        rc = 1;
    }
    if (pics.size() != run.outputs) {
        Say("FAIL the decoder returned %zu pictures for %u access units", pics.size(), run.outputs);
        rc = 1;
    }
    if (run.keyFrames == 0) {
        Say("FAIL no output sample was marked as a clean point");
        rc = 1;
    }
    // With low latency cleared the transform is meant to hold a picture back, so a run that produced
    // its first access unit from the first input alone did not pipeline and this case stopped covering
    // the drain path that flushes one.
    if (run.needBeforeFirstOutput < 2) {
        Say("FAIL the transform produced its first access unit without asking for a second picture, "
            "with low latency cleared");
        rc = 1;
    }
    if (settingFailures != 0 || moduleLockFailures != 0) {
        rc = 1;
    }
    const std::wstring path = o.outDir + L"\\mfthost-mft.264";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f != nullptr) {
        fwrite(stream.data(), 1, stream.size(), f);
        fclose(f);
        Say("stream written to %ls", path.c_str());
    }
    dec.Shutdown();
    return rc;
}

// ---------------------------------------------------------------- comparison with the inbox encoder

namespace {

struct EncodeResult {
    bool ok = false;
    uint64_t bytes = 0;
    double ms = 0.0;
    double psnrY = 0.0;
    double psnrCb = 0.0;
    double psnrCr = 0.0;
    uint32_t pictures = 0;
    std::vector<uint8_t> stream;
};

// Runs the inbox H264 Encoder MFT over a sequence of NV12 pictures. Synchronous model.
HRESULT RunInboxEncoder(const Options& o, const std::vector<std::vector<uint8_t>>& nv12,
                        EncodeResult* result)
{
    ComPtr<IMFTransform> mft;
    HRESULT hr = CoCreateInstance(kInboxH264EncoderClsid, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMFTransform), reinterpret_cast<void**>(&mft));
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFMediaType> outType;
    hr = MakeVideoType(MFVideoFormat_H264, o.width, o.height, o.fps, &outType);
    if (SUCCEEDED(hr)) { hr = outType->SetUINT32(MF_MT_AVG_BITRATE, o.bitrate); }
    if (SUCCEEDED(hr)) {
        hr = outType->SetUINT32(MF_MT_MPEG2_PROFILE,
                                static_cast<UINT32>(eAVEncH264VProfile_ConstrainedBase));
    }
    if (SUCCEEDED(hr)) { hr = mft->SetOutputType(0, outType.Get(), 0); }
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IMFMediaType> inType;
    hr = MakeVideoType(MFVideoFormat_NV12, o.width, o.height, o.fps, &inType);
    if (SUCCEEDED(hr)) { hr = mft->SetInputType(0, inType.Get(), 0); }
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(mft->QueryInterface(__uuidof(ICodecAPI), reinterpret_cast<void**>(&codec)))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = static_cast<ULONG>(eAVEncCommonRateControlMode_CBR);
        codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.ulVal = o.bitrate;
        codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        v.ulVal = o.gop;
        codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        v.ulVal = 0;
        codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        VariantClear(&v);
    }
    MFT_OUTPUT_STREAM_INFO osi = {};
    mft->GetOutputStreamInfo(0, &osi);
    const bool provides = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                          MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    const DWORD outBytes = osi.cbSize ? osi.cbSize : (o.width * o.height * 2u);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    const int64_t duration = 10000000LL / o.fps;
    auto pull = [&]() -> HRESULT {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER buf = {};
            ComPtr<IMFSample> allocated;
            if (!provides) {
                HRESULT h = MFCreateSample(&allocated);
                if (FAILED(h)) {
                    return h;
                }
                ComPtr<IMFMediaBuffer> b;
                h = MFCreateMemoryBuffer(outBytes, &b);
                if (FAILED(h)) {
                    return h;
                }
                h = allocated->AddBuffer(b.Get());
                if (FAILED(h)) {
                    return h;
                }
                buf.pSample = allocated.Get();
            }
            DWORD status = 0;
            HRESULT h = mft->ProcessOutput(0, 1, &buf, &status);
            if (h == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return S_OK;
            }
            if (h == MF_E_TRANSFORM_STREAM_CHANGE) {
                continue;
            }
            if (FAILED(h)) {
                return h;
            }
            // The same ownership rule as H264Decoder::PullAll: a sample we allocated stays owned by
            // `allocated`, so it is copied, not taken over.
            ComPtr<IMFSample> s;
            if (buf.pSample == allocated.Get()) {
                s.CopyFrom(buf.pSample);
            } else {
                s.Attach(buf.pSample);
            }
            if (buf.pEvents != nullptr) {
                buf.pEvents->Release();
            }
            ComPtr<IMFMediaBuffer> b;
            h = s->ConvertToContiguousBuffer(&b);
            if (FAILED(h)) {
                return h;
            }
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            h = b->Lock(&data, &maxLen, &curLen);
            if (FAILED(h)) {
                return h;
            }
            result->stream.insert(result->stream.end(), data, data + curLen);
            result->bytes += curLen;
            ++result->pictures;
            b->Unlock();
        }
    };

    // The clock starts after the pictures --timing-skip excludes, exactly as our own loop in
    // RunCompare does, so the two columns of the table measure the same set of pictures.
    double t0 = NowMs();
    for (uint32_t i = 0; i < nv12.size(); ++i) {
        if (i == o.timingSkip) {
            t0 = NowMs();
        }
        ComPtr<IMFSample> s;
        hr = MakeMemorySample(nv12[i].data(), nv12[i].size(), static_cast<int64_t>(i) * duration,
                              duration, &s);
        if (FAILED(hr)) {
            return hr;
        }
        hr = mft->ProcessInput(0, s.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            hr = pull();
            if (FAILED(hr)) {
                return hr;
            }
            hr = mft->ProcessInput(0, s.Get(), 0);
        }
        if (FAILED(hr)) {
            return hr;
        }
        hr = pull();
        if (FAILED(hr)) {
            return hr;
        }
    }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    hr = pull();
    result->ms = NowMs() - t0;
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    result->ok = SUCCEEDED(hr);
    return hr;
}

// Decodes a stream and measures it against the sources it was made from.
HRESULT MeasureAgainstSource(const Options& o, const std::vector<Picture>& sources,
                             EncodeResult* r)
{
    H264Decoder dec;
    HRESULT hr = dec.Initialize(o.width, o.height);
    if (FAILED(hr)) {
        return hr;
    }
    // The stream is a sequence of access units; the decoder is happy to take it in one piece per
    // start code boundary, but feeding the whole thing at once is simpler and equally valid.
    hr = dec.Feed(r->stream.data(), r->stream.size(), 0);
    if (SUCCEEDED(hr)) {
        hr = dec.Drain();
    }
    if (FAILED(hr)) {
        return hr;
    }
    std::vector<Picture>& pics = dec.Pictures();
    const size_t n = (pics.size() < sources.size()) ? pics.size() : sources.size();
    double sy = 0.0, sc = 0.0, sr = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sy += PlanePsnr(sources[i].y.data(), pics[i].y.data(), sources[i].y.size());
        sc += PlanePsnr(sources[i].cb.data(), pics[i].cb.data(), sources[i].cb.size());
        sr += PlanePsnr(sources[i].cr.data(), pics[i].cr.data(), sources[i].cr.size());
    }
    if (n != 0) {
        r->psnrY = sy / n;
        r->psnrCb = sc / n;
        r->psnrCr = sr / n;
    }
    r->pictures = static_cast<uint32_t>(pics.size());
    dec.Shutdown();
    return S_OK;
}

} // namespace

int RunCompare(const Options& o)
{
    printf("compare: our encoder against the inbox H264 Encoder MFT, same source, CBR %u bit/s\n",
           o.bitrate);

    std::vector<Picture> sources(o.frames);
    std::vector<std::vector<uint8_t>> nv12(o.frames);
    for (uint32_t i = 0; i < o.frames; ++i) {
        sources[i].Allocate(o.width, o.height);
        MakeSyntheticPicture(sources[i], i);
        I420ToNv12(sources[i], nv12[i]);
    }

    // Ours, through the encoder core, CBR at the same bitrate.
    EncodeResult ours;
    {
        EncoderConfig cfg;
        cfg.width = o.width;
        cfg.height = o.height;
        cfg.fpsNum = o.fps;
        cfg.fpsDen = 1;
        cfg.gopSize = o.gop;
        cfg.meanBitRate = o.bitrate;
        cfg.rateControl = RateControl::Cbr;
        cfg.deblocking = o.deblock;
        // The test's own device (mfthost.h): the transform creates one on the BC-250 adapter alone.
        ComPtr<ID3D11Device> device;
        HRESULT hr = CreateTestDevice(&device);
        if (FAILED(hr)) {
            SayHr("CreateTestDevice", hr);
            return 2;
        }
        Encoder enc;
        hr = enc.Initialize(device.Get(), cfg);
        if (FAILED(hr)) {
            SayHr("Encoder::Initialize", hr);
            return 2;
        }
        std::vector<uint8_t> frame;
        double t0 = NowMs();
        for (uint32_t i = 0; i < o.frames; ++i) {
            if (i == o.timingSkip) {
                t0 = NowMs();
            }
            GpuFrameInput in;
            in.kind = InputKind::Planar8;
            in.planeY = sources[i].y.data();
            in.planeCb = sources[i].cb.data();
            in.planeCr = sources[i].cr.data();
            in.pitchY = o.width;
            in.pitchC = o.width / 2;
            FrameStats st;
            hr = enc.EncodeFrame(in, false, frame, &st);
            if (FAILED(hr)) {
                SayHr("EncodeFrame", hr);
                return 2;
            }
            ours.stream.insert(ours.stream.end(), frame.begin(), frame.end());
            ours.bytes += st.bytes;
        }
        ours.ms = NowMs() - t0;
        ours.ok = true;
        enc.Shutdown();
    }
    HRESULT hr = MeasureAgainstSource(o, sources, &ours);
    if (FAILED(hr)) {
        SayHr("decoding our own stream", hr);
        return 2;
    }

    EncodeResult inbox;
    hr = RunInboxEncoder(o, nv12, &inbox);
    if (FAILED(hr)) {
        Say("the inbox encoder failed 0x%08lX; the comparison column is unavailable",
            static_cast<unsigned long>(hr));
    } else {
        hr = MeasureAgainstSource(o, sources, &inbox);
        if (FAILED(hr)) {
            Say("decoding the inbox stream failed 0x%08lX", static_cast<unsigned long>(hr));
            inbox.ok = false;
        }
    }

    // The byte and PSNR rows are over every picture; the two time rows are over the pictures both
    // encoders were timed on, which --timing-skip may have shortened.
    const uint32_t timedFrames = (o.frames > o.timingSkip) ? (o.frames - o.timingSkip) : 1u;
    printf("\n");
    printf("  %-28s %14s %14s\n", "", "ours", "inbox H264");
    printf("  %-28s %14llu %14llu\n", "bytes", static_cast<unsigned long long>(ours.bytes),
           static_cast<unsigned long long>(inbox.bytes));
    printf("  %-28s %14.0f %14.0f\n", "bit/s at the nominal rate",
           (ours.bytes * 8.0 * o.fps) / o.frames, (inbox.bytes * 8.0 * o.fps) / o.frames);
    if (o.timingSkip != 0) {
        printf("  %-28s %14u %14u\n", "pictures timed", timedFrames, timedFrames);
    }
    printf("  %-28s %14.2f %14.2f\n", "ms per picture", ours.ms / timedFrames,
           inbox.ms / timedFrames);
    printf("  %-28s %14.1f %14.1f\n", "pictures per second", 1000.0 * timedFrames / ours.ms,
           inbox.ms > 0.0 ? 1000.0 * timedFrames / inbox.ms : 0.0);
    printf("  %-28s %14.2f %14.2f\n", "PSNR Y (dB)", ours.psnrY, inbox.psnrY);
    printf("  %-28s %14.2f %14.2f\n", "PSNR Cb (dB)", ours.psnrCb, inbox.psnrCb);
    printf("  %-28s %14.2f %14.2f\n", "PSNR Cr (dB)", ours.psnrCr, inbox.psnrCr);
    printf("  %-28s %14u %14u\n", "pictures decoded", ours.pictures, inbox.pictures);
    printf("\n");

    int rc = 0;
    if (ours.pictures != o.frames) {
        Say("FAIL our stream decoded to %u of %u pictures", ours.pictures, o.frames);
        rc = 1;
    }
    return rc;
}

// ---------------------------------------------------------------- sink writer

int RunSinkWriter(const Options& o)
{
    printf("sinkwriter: a Media Foundation sink writer to .mp4 with our transform registered "
           "locally\n");

    ComPtr<IClassFactory> factory;
    factory.Attach(new (std::nothrow) TestFactory());
    if (!factory) {
        Say("out of memory");
        return 2;
    }
    HRESULT hr = RegisterLocal(factory.Get());
    if (FAILED(hr)) {
        SayHr("MFTRegisterLocal", hr);
        return 2;
    }
    const long before = Bc250H264EncodedPictureCount();

    const std::wstring path = o.outDir + L"\\mfthost-sinkwriter.mp4";
    DeleteFileW(path.c_str());

    // A Direct3D 11 device for the writer to pass on, which is what a recording client gives it. Our
    // transform creates a device only on the BC-250 adapter and answers DXGI_ERROR_NOT_FOUND on any
    // other machine, by design, so without a device manager this case fails at the first WriteSample
    // on every development PC - which is how it failed, 0x887A0002 at BeginWriting, until this was
    // added. MF_SINK_WRITER_D3D_MANAGER is the writer's own name for the manager it forwards to the
    // transforms in its chain through MFT_MESSAGE_SET_D3D_MANAGER.
    ComPtr<ID3D11Device> device;
    hr = CreateTestDevice(&device);
    if (FAILED(hr)) {
        SayHr("CreateTestDevice", hr);
        UnregisterLocal();
        return 2;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D10Multithread),
                                         reinterpret_cast<void**>(&mt)))) {
        // Required of a device behind a device manager: the writer's own worker thread and ours both
        // reach this device.
        mt->SetMultithreadProtected(TRUE);
    }
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &manager);
    if (SUCCEEDED(hr)) {
        hr = manager->ResetDevice(device.Get(), token);
    }
    if (FAILED(hr)) {
        SayHr("MFCreateDXGIDeviceManager/ResetDevice", hr);
        UnregisterLocal();
        return 2;
    }

    ComPtr<IMFAttributes> attrs;
    hr = MFCreateAttributes(&attrs, 4);
    if (SUCCEEDED(hr)) {
        hr = attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, o.noHwTransforms ? 0u : 1u);
    }
    if (SUCCEEDED(hr)) { hr = attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, 1); }
    if (SUCCEEDED(hr)) { hr = attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager.Get()); }
    if (FAILED(hr)) {
        SayHr("MFCreateAttributes", hr);
        UnregisterLocal();
        return 2;
    }
    ComPtr<IMFSinkWriter> writer;
    Say("stage: MFCreateSinkWriterFromURL");
    hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer);
    if (FAILED(hr)) {
        SayHr("MFCreateSinkWriterFromURL", hr);
        UnregisterLocal();
        return 2;
    }
    ComPtr<IMFMediaType> outType;
    hr = MakeVideoType(MFVideoFormat_H264, o.width, o.height, o.fps, &outType);
    if (SUCCEEDED(hr)) { hr = outType->SetUINT32(MF_MT_AVG_BITRATE, o.bitrate); }
    DWORD stream = 0;
    Say("stage: AddStream");
    if (SUCCEEDED(hr)) { hr = writer->AddStream(outType.Get(), &stream); }
    if (FAILED(hr)) {
        SayHr("AddStream(H264)", hr);
        UnregisterLocal();
        return 2;
    }
    ComPtr<IMFMediaType> inType;
    hr = MakeVideoType(MFVideoFormat_NV12, o.width, o.height, o.fps, &inType);
    Say("stage: SetInputMediaType");
    if (SUCCEEDED(hr)) { hr = writer->SetInputMediaType(stream, inType.Get(), nullptr); }
    if (FAILED(hr)) {
        SayHr("SetInputMediaType(NV12)", hr);
        UnregisterLocal();
        return 2;
    }
    // Which transforms the writer put in the chain. Worth printing on every run: on this host the
    // writer sometimes stalls for minutes inside MFCreateSinkWriterFromURL or BeginWriting while
    // bringing up another vendor's hardware encoder, and the chain says whether ours was chosen.
    {
        ComPtr<IMFSinkWriterEx> ex;
        if (SUCCEEDED(writer->QueryInterface(__uuidof(IMFSinkWriterEx),
                                             reinterpret_cast<void**>(&ex)))) {
            for (DWORD ti = 0; ti < 8; ++ti) {
                GUID category = GUID_NULL;
                ComPtr<IMFTransform> t;
                if (FAILED(ex->GetTransformForStream(stream, ti, &category, &t)) || !t) {
                    break;
                }
                ComPtr<IMFAttributes> ta;
                wchar_t* turl = nullptr;
                UINT32 tlen = 0;
                bool mine = false;
                if (SUCCEEDED(t->GetAttributes(&ta)) &&
                    SUCCEEDED(ta->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &turl,
                                                     &tlen))) {
                    mine = (wcscmp(turl, L"amdgpu_wddm://h264-encoder/0") == 0);
                    CoTaskMemFree(turl);
                }
                printf("  chain [%lu] category {%08lX-...} %s\n", ti,
                       static_cast<unsigned long>(category.Data1), mine ? "ours" : "not ours");
            }
        }
    }
    Say("stage: BeginWriting");
    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        SayHr("BeginWriting", hr);
        UnregisterLocal();
        return 2;
    }

    const uint32_t frames = (o.frames < 60u) ? o.frames : 60u;
    const int64_t duration = 10000000LL / o.fps;
    Picture src;
    src.Allocate(o.width, o.height);
    std::vector<uint8_t> nv12;
    const double t0 = NowMs();
    for (uint32_t i = 0; i < frames; ++i) {
        MakeSyntheticPicture(src, i);
        I420ToNv12(src, nv12);
        ComPtr<IMFSample> s;
        hr = MakeMemorySample(nv12.data(), nv12.size(), static_cast<int64_t>(i) * duration,
                              duration, &s);
        if (FAILED(hr)) {
            SayHr("MakeMemorySample", hr);
            break;
        }
        if (o.verbose) { Say("stage: WriteSample %u", i); }
        hr = writer->WriteSample(stream, s.Get());
        if (FAILED(hr)) {
            SayHr("WriteSample", hr);
            break;
        }
    }
    Say("stage: Finalize");
    const HRESULT fin = writer->Finalize();
    const double ms = NowMs() - t0;
    writer.Reset();
    const long after = Bc250H264EncodedPictureCount();
    UnregisterLocal();

    if (FAILED(fin)) {
        SayHr("Finalize", fin);
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    uint64_t size = 0;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    Say("wrote %u pictures in %.1f ms, %ls is %llu bytes", frames, ms, path.c_str(),
        static_cast<unsigned long long>(size));
    Say("pictures that went through our transform: %ld", after - before);

    int rc = 0;
    if (size == 0) {
        Say("FAIL the sink writer produced an empty file");
        rc = 1;
    }
    if (after - before == 0) {
        Say("the sink writer chose a different encoder: our transform encoded nothing. The file is "
            "still valid H.264, but this run says nothing about our MFT.");
        // On a machine without a BC-250 that outcome is the designed one and not a defect, so the case
        // says what it could not test instead of failing for good. Both ways out are closed here: with
        // the device manager above the platform prefers the hardware encoder of the adapter the client
        // handed over, which is not ours, and without it our transform is chosen and then answers
        // DXGI_ERROR_NOT_FOUND, because it creates a device on the BC-250 adapter and on no other. On
        // unit A the two agree, because there the client's adapter is the one our transform wants.
        if (!HaveBc250Adapter()) {
            Say("no BC-250 in this machine, so no sink writer chain can reach our transform: this "
                "case is not applicable here and is not counted as a failure");
            rc = 0;
        } else {
            rc = 1;
        }
    } else if (static_cast<uint32_t>(after - before) != frames) {
        Say("our transform encoded %ld of %u pictures", after - before, frames);
    }
    return rc;
}

// ---------------------------------------------------------------- Direct3D 11 input shapes

namespace {

struct TextureCase {
    const char* name;
    const GUID* subtype;
    uint32_t arraySize;
    uint32_t slice;
};

// Drives our transform through the asynchronous contract once, with one Direct3D 11 input shape, and
// decodes every access unit with the inbox decoder. Returns 0 on success.
int RunOneTextureCase(const Options& o, const TextureCase& tc, ID3D11Device* device,
                      IMFDXGIDeviceManager* manager)
{
    const bool nv12 = (*tc.subtype == MFVideoFormat_NV12);
    Say("case %s: %s, array size %u, slice %u", tc.name, nv12 ? "NV12" : "BGRA", tc.arraySize,
        tc.slice);

    ComPtr<IMFTransform> mft;
    bool asyncFlag = false;
    HRESULT hr = FindOurTransformByEnumeration(&mft, &asyncFlag);
    if (FAILED(hr) || !mft) {
        SayHr("MFTEnumEx did not find the locally registered transform", hr);
        return 2;
    }
    ComPtr<IMFAttributes> attrs;
    hr = mft->GetAttributes(&attrs);
    if (SUCCEEDED(hr)) { hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 1); }
    if (SUCCEEDED(hr)) {
        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                 reinterpret_cast<ULONG_PTR>(manager));
    }
    if (FAILED(hr)) {
        SayHr("unlock and SET_D3D_MANAGER", hr);
        return 2;
    }

    ComPtr<IMFMediaType> outType;
    hr = MakeVideoType(MFVideoFormat_H264, o.width, o.height, o.fps, &outType);
    if (SUCCEEDED(hr)) { hr = outType->SetUINT32(MF_MT_AVG_BITRATE, o.bitrate); }
    if (SUCCEEDED(hr)) { hr = outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, o.gop); }
    if (SUCCEEDED(hr)) { hr = mft->SetOutputType(0, outType.Get(), 0); }
    ComPtr<IMFMediaType> inType;
    if (SUCCEEDED(hr)) { hr = MakeVideoType(*tc.subtype, o.width, o.height, o.fps, &inType); }
    if (SUCCEEDED(hr)) { hr = mft->SetInputType(0, inType.Get(), 0); }
    if (FAILED(hr)) {
        SayHr("media types", hr);
        return 2;
    }

    Pattern pattern;
    Nv12Source nv12src;
    ID3D11Texture2D* texture = nullptr;
    if (nv12) {
        hr = nv12src.Initialize(device, o.width, o.height, tc.arraySize, tc.slice);
        texture = nv12src.Texture();
        if (SUCCEEDED(hr)) {
            Say("  the driver accepted the NV12 texture with bind flags 0x%X%s",
                nv12src.BindFlags(),
                (nv12src.BindFlags() & D3D11_BIND_SHADER_RESOURCE) ? "" :
                    " (not shader bindable, so the encoder has to copy the slice)");
        }
    } else {
        hr = pattern.Initialize(device, o.width, o.height, tc.arraySize, tc.slice);
        texture = pattern.Texture();
    }
    if (FAILED(hr)) {
        SayHr("the source texture", hr);
        return 2;
    }

    ComPtr<IMFMediaEventGenerator> events;
    hr = mft->QueryInterface(__uuidof(IMFMediaEventGenerator), reinterpret_cast<void**>(&events));
    if (SUCCEEDED(hr)) { hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); }
    if (SUCCEEDED(hr)) { hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0); }
    if (FAILED(hr)) {
        SayHr("begin streaming", hr);
        return 2;
    }

    H264Decoder dec;
    hr = dec.Initialize(o.width, o.height);
    if (FAILED(hr)) {
        SayHr("the inbox decoder MFT", hr);
        return 2;
    }

    const int64_t duration = 10000000LL / o.fps;
    uint32_t fed = 0, outputs = 0;
    bool endOfStream = false, drained = false, failed = false;
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(events->GetEvent(0, &ev))) {
            break;
        }
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            if (fed < o.frames) {
                hr = nv12 ? nv12src.Write(fed) : pattern.Draw(fed);
                ComPtr<IMFMediaBuffer> buffer;
                if (SUCCEEDED(hr)) {
                    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, tc.slice,
                                                   FALSE, &buffer);
                }
                ComPtr<IMFSample> sample;
                if (SUCCEEDED(hr)) { hr = MFCreateSample(&sample); }
                if (SUCCEEDED(hr)) { hr = sample->AddBuffer(buffer.Get()); }
                if (SUCCEEDED(hr)) {
                    hr = sample->SetSampleTime(static_cast<int64_t>(fed) * duration);
                }
                if (SUCCEEDED(hr)) { hr = sample->SetSampleDuration(duration); }
                if (SUCCEEDED(hr)) { hr = mft->ProcessInput(0, sample.Get(), 0); }
                if (FAILED(hr)) {
                    SayHr("ProcessInput", hr);
                    failed = true;
                    break;
                }
                ++fed;
            } else if (!endOfStream) {
                endOfStream = true;
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            }
        } else if (type == METransformHaveOutput) {
            MFT_OUTPUT_DATA_BUFFER buf = {};
            DWORD status = 0;
            hr = mft->ProcessOutput(0, 1, &buf, &status);
            if (FAILED(hr)) {
                SayHr("ProcessOutput", hr);
                failed = true;
                break;
            }
            ComPtr<IMFSample> out;
            out.Attach(buf.pSample);
            if (buf.pEvents != nullptr) {
                buf.pEvents->Release();
            }
            ComPtr<IMFMediaBuffer> b;
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            hr = out->ConvertToContiguousBuffer(&b);
            if (SUCCEEDED(hr)) { hr = b->Lock(&data, &maxLen, &curLen); }
            if (FAILED(hr)) {
                SayHr("reading the output sample", hr);
                failed = true;
                break;
            }
            const HRESULT fh = dec.Feed(data, curLen, static_cast<int64_t>(outputs) * duration);
            b->Unlock();
            ++outputs;
            if (FAILED(fh)) {
                Say("FAIL the decoder rejected output %u: 0x%08lX", outputs - 1,
                    static_cast<unsigned long>(fh));
                failed = true;
                break;
            }
        } else if (type == METransformDrainComplete) {
            drained = true;
            break;
        } else if (type == MEError) {
            HRESULT st = S_OK;
            ev->GetStatus(&st);
            Say("FAIL MEError 0x%08lX", static_cast<unsigned long>(st));
            failed = true;
            break;
        }
    }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    dec.Drain();

    ComPtr<IMFShutdown> shutdown;
    if (SUCCEEDED(mft->QueryInterface(__uuidof(IMFShutdown), reinterpret_cast<void**>(&shutdown)))) {
        shutdown->Shutdown();
    }
    mft.Reset();

    if (failed) {
        return 1;
    }
    if (outputs != o.frames || !drained) {
        Say("FAIL %u of %u pictures came out, drain %s", outputs, o.frames,
            drained ? "complete" : "NOT SIGNALLED");
        return 1;
    }
    std::vector<Picture>& pics = dec.Pictures();
    if (pics.empty()) {
        Say("FAIL the inbox decoder produced no picture");
        return 1;
    }
    Say("  %u pictures encoded and %zu decoded by the inbox decoder", outputs, pics.size());

    // For the NV12 cases the source is our own synthetic picture, so the decoded luma can be
    // compared with it directly. That is what proves the right array slice was read: a wrong slice
    // is a never written texture, which decodes flat and scores far below any real match. The BGRA
    // cases go through the shader's colour conversion, so only their mean level is checked.
    if (nv12) {
        Picture source;
        source.Allocate(o.width, o.height);
        MakeSyntheticPicture(source, o.frames - 1);
        const Picture& last = pics.back();
        if (last.width != o.width || last.height != o.height) {
            Say("FAIL the decoded picture is %ux%u, expected %ux%u", last.width, last.height,
                o.width, o.height);
            return 1;
        }
        const double psnr = PlanePsnr(source.y.data(), last.y.data(), source.y.size());
        Say("  PSNR(Y) of the last decoded picture against the written slice: %6.2f dB", psnr);
        if (psnr < 25.0) {
            Say("FAIL the decoded picture does not match the slice that was written: the encoder "
                "read the wrong array slice, or the chroma plane was not imported");
            return 1;
        }
    } else {
        double mean = 0.0;
        for (uint8_t v : pics.back().y) {
            mean += v;
        }
        mean /= static_cast<double>(pics.back().y.size() ? pics.back().y.size() : 1);
        Say("  mean decoded luma %5.1f", mean);
        if (mean < 8.0) {
            Say("FAIL the decoded picture is black: the encoder read a slice that was never drawn");
            return 1;
        }
    }
    return 0;
}

} // namespace

int RunTextureInput(const Options& o)
{
    printf("texin: Direct3D 11 texture input, including the array slices Game Bar hands out\n");

    ComPtr<IClassFactory> factory;
    factory.Attach(new (std::nothrow) TestFactory());
    if (!factory) {
        return 2;
    }
    HRESULT hr = RegisterLocal(factory.Get());
    if (FAILED(hr)) {
        SayHr("MFTRegisterLocal", hr);
        return 2;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                           D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        SayHr("D3D11CreateDevice", hr);
        UnregisterLocal();
        return 2;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D10Multithread),
                                         reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
    }
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &manager);
    if (SUCCEEDED(hr)) { hr = manager->ResetDevice(device.Get(), token); }
    if (FAILED(hr)) {
        SayHr("MFCreateDXGIDeviceManager/ResetDevice", hr);
        UnregisterLocal();
        return 2;
    }

    // The real time client contract, which the frame server and the capture engine query on every
    // transform of a real time topology before they start it.
    {
        ComPtr<IMFTransform> probe;
        bool asyncFlag = false;
        if (SUCCEEDED(FindOurTransformByEnumeration(&probe, &asyncFlag)) && probe) {
            ComPtr<IMFRealTimeClientEx> rt;
            if (SUCCEEDED(probe->QueryInterface(__uuidof(IMFRealTimeClientEx),
                                                reinterpret_cast<void**>(&rt)))) {
                DWORD task = 0;
                const HRESULT r1 = rt->RegisterThreadsEx(&task, L"Capture", 0);
                const HRESULT r2 = rt->SetWorkQueueEx(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0);
                const HRESULT r3 = rt->UnregisterThreads();
                if (FAILED(r1) || FAILED(r2) || FAILED(r3)) {
                    Say("FAIL IMFRealTimeClientEx answered 0x%08lX 0x%08lX 0x%08lX",
                        static_cast<unsigned long>(r1), static_cast<unsigned long>(r2),
                        static_cast<unsigned long>(r3));
                    UnregisterLocal();
                    return 1;
                }
                Say("IMFRealTimeClientEx: threads registered, work queue set, threads released");
            } else {
                Say("FAIL the transform does not expose IMFRealTimeClientEx");
                UnregisterLocal();
                return 1;
            }
            ComPtr<IMFShutdown> sd;
            if (SUCCEEDED(probe->QueryInterface(__uuidof(IMFShutdown),
                                                reinterpret_cast<void**>(&sd)))) {
                sd->Shutdown();
            }
        }
    }

    const TextureCase cases[] = {
        { "bgra-plain", &MFVideoFormat_ARGB32, 1, 0 },
        { "bgra-slice", &MFVideoFormat_ARGB32, 4, 2 },
        { "nv12-plain", &MFVideoFormat_NV12,   1, 0 },
        { "nv12-slice", &MFVideoFormat_NV12,   4, 2 },
    };
    uint32_t failures = 0;
    for (const TextureCase& tc : cases) {
        if (RunOneTextureCase(o, tc, device.Get(), manager.Get()) != 0) {
            ++failures;
        }
    }
    UnregisterLocal();
    Say("texture input: %u of %zu cases passed", static_cast<uint32_t>(_countof(cases)) - failures,
        _countof(cases));
    return failures == 0 ? 0 : 1;
}

} // namespace test
} // namespace bc250h264
