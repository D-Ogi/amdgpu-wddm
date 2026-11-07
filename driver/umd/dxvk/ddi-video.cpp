// SPDX-License-Identifier: MIT
#include "ddi-video.h"
#include "ddi-resource.h"
#include "ddi-status-note.h"
#include <cstddef>
#include <cstring>
#include <vector>
namespace bc250::umd {
// The three table layouts begin with the same members in the same order: the 2.0 table is the 11.1 table and
// thirteen more entries, and the 2.1 table is the 2.0 table and two more. One 2.1 table therefore fills each of them
// with a copy of its first bytes. These checks hold the WDK to that.
static_assert(sizeof(D3D11_1DDI_VIDEODEVICEFUNCS)==offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnGetDataForNewHardwareKey));
static_assert(sizeof(D3DWDDM2_0DDI_VIDEODEVICEFUNCS)==offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnVideoProcessorSetOutputHDRMetaData));
static_assert(offsetof(D3D11_1DDI_VIDEODEVICEFUNCS,pfnGetCaptureHandle)==offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnGetCaptureHandle));
static_assert(offsetof(D3D11_1DDI_VIDEODEVICEFUNCS,pfnVideoProcessorBlt)==offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnVideoProcessorBlt));
static_assert(offsetof(D3DWDDM2_0DDI_VIDEODEVICEFUNCS,pfnGetDataForNewHardwareKey)==offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnGetDataForNewHardwareKey));
static_assert(offsetof(D3DWDDM2_0DDI_VIDEODEVICEFUNCS,pfnGetCryptoSessionPrivateDataSize)==offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnGetCryptoSessionPrivateDataSize));
// The input argument of each sub ID: a relocation flag and the runtime's table, in the same place.
static_assert(sizeof(D3D11_1DDI_VIDEO_INPUT)==sizeof(D3DWDDM2_0DDI_VIDEO_INPUT) && sizeof(D3D11_1DDI_VIDEO_INPUT)==sizeof(D3DWDDM2_1DDI_VIDEO_INPUT));
static_assert(offsetof(D3D11_1DDI_VIDEO_INPUT,p11VideoDeviceFuncs)==offsetof(D3DWDDM2_0DDI_VIDEO_INPUT,pWDDM2_0VideoDeviceFuncs));
static_assert(offsetof(D3D11_1DDI_VIDEO_INPUT,p11VideoDeviceFuncs)==offsetof(D3DWDDM2_1DDI_VIDEO_INPUT,pWDDM2_1VideoDeviceFuncs));
// The engine's argument types are the DDI types with other names. The conversions below copy field by field; these
// checks catch a header that changes one side.
static_assert(sizeof(D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE)==sizeof(D3D11_VIDEO_PROCESSOR_COLOR_SPACE));
static_assert(sizeof(D3D11_1DDI_VIDEO_COLOR)==sizeof(D3D11_VIDEO_COLOR));
static_assert(UINT(D3D11_1DDI_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST)==UINT(D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST));
static_assert(UINT(D3D11_1DDI_VIDEO_USAGE_OPTIMAL_QUALITY)==UINT(D3D11_VIDEO_USAGE_OPTIMAL_QUALITY));
static_assert(UINT(D3D11_1DDI_VIDEO_PROCESSOR_ROTATION_270)==UINT(D3D11_VIDEO_PROCESSOR_ROTATION_270));
static_assert(UINT(D3DDDI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709)==UINT(DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709));

SIZE_T video_table_size(UINT32 subId) {
    switch (subId) {
    case D3D11_1DDI_VIDEO_FUNCTIONS: return sizeof(D3D11_1DDI_VIDEODEVICEFUNCS);
    case D3DWDDM2_0DDI_VIDEO_FUNCTIONS: return sizeof(D3DWDDM2_0DDI_VIDEODEVICEFUNCS);
    case D3DWDDM2_1DDI_VIDEO_FUNCTIONS: return sizeof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS);
    default: return 0;
    }
}
D3D11_VIDEO_PROCESSOR_CONTENT_DESC convert_video_content(const D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC &s) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC d{};
    d.InputFrameFormat=static_cast<D3D11_VIDEO_FRAME_FORMAT>(s.InputFrameFormat);
    d.InputFrameRate=s.InputFrameRate;
    d.InputWidth=s.InputWidth; d.InputHeight=s.InputHeight;
    d.OutputFrameRate=s.OutputFrameRate;
    d.OutputWidth=s.OutputWidth; d.OutputHeight=s.OutputHeight;
    d.Usage=static_cast<D3D11_VIDEO_USAGE>(s.Usage);
    return d;
}
D3D11_VIDEO_PROCESSOR_COLOR_SPACE convert_video_space(const D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE &s) {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE d{};
    d.Usage=s.Usage; d.RGB_Range=s.RGB_Range; d.YCbCr_Matrix=s.YCbCr_Matrix; d.YCbCr_xvYCC=s.YCbCr_xvYCC;
    d.Nominal_Range=s.Nominal_Range;
    return d;
}
D3D11_VIDEO_COLOR convert_video_color(const D3D11_1DDI_VIDEO_COLOR &s) {
    D3D11_VIDEO_COLOR d{};
    d.RGBA.R=s.RGBA.R; d.RGBA.G=s.RGBA.G; d.RGBA.B=s.RGBA.B; d.RGBA.A=s.RGBA.A; // YCbCrA aliases the same floats
    return d;
}
bool plan_video_input_view(const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW &s,DXGI_FORMAT format,
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC &out) {
    const auto *row=video_format_row(format);
    if (!row || !row->input || s.ArraySize>1) return false;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
    d.FourCC=s.FourCC;
    d.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice=s.MipSlice;
    d.Texture2D.ArraySlice=s.FirstArraySlice;
    out=d; return true;
}
bool plan_video_output_view(const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW &s,DXGI_FORMAT format,
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC &out) {
    const auto *row=video_format_row(format);
    if (!row || !row->output || s.ArraySize>1) return false;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
    if (s.FirstArraySlice) {
        d.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice=s.MipSlice; d.Texture2DArray.FirstArraySlice=s.FirstArraySlice; d.Texture2DArray.ArraySize=1;
    } else {
        d.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice=s.MipSlice;
    }
    out=d; return true;
}
// The statuses of the entries that cannot succeed. A status is one the entry's page names. Six pages name no status
// (CreateVideoDecoderOutputView, CreateCryptoSession, CreateAuthenticatedChannel, ConfigureAuthenticatedChannel and
// the two downsampling entries): their create entries answer E_OUTOFMEMORY, the status of every sibling create page,
// and the others E_INVALIDARG, the status of their sibling channel and session entries.
// GetContentProtectionCaps and GetCryptoKeyExchangeType are not in the table: their pages name D3DERR_INVALID_CRYPTO,
// which no header of the WDK or the SDK defines, so they answer S_OK with no protection capability and no key
// exchange type.
const VideoRefusal kVideoRefusals[]={
    {"CreateVideoDecoder","PFND3D11_1DDI_CREATEVIDEODECODER",E_OUTOFMEMORY},
    {"VideoDecoderExtension","PFND3D11_1DDI_VIDEODECODEREXTENSION",E_INVALIDARG},
    {"VideoDecoderBeginFrame","PFND3D11_1DDI_VIDEODECODERBEGINFRAME",E_OUTOFMEMORY},
    {"VideoDecoderSubmitBuffers","PFND3D11_1DDI_VIDEODECODERSUBMITBUFFERS",E_OUTOFMEMORY},
    {"VideoDecoderSubmitBuffers1","PFND3DWDDM2_0DDI_VIDEODECODERSUBMITBUFFERS1",E_OUTOFMEMORY},
    {"VideoDecoderGetHandle","PFND3D11_1DDI_VIDEODECODERGETHANDLE",E_OUTOFMEMORY},
    {"VideoDecoderEnableDownsampling","PFND3DWDDM2_0DDI_VIDEODECODERENABLEDOWNSAMPLING",E_INVALIDARG},
    {"VideoDecoderUpdateDownsampling","PFND3DWDDM2_0DDI_VIDEODECODERUPDATEDOWNSAMPLING",E_INVALIDARG},
    {"CreateVideoDecoderOutputView","PFND3D11_1DDI_CREATEVIDEODECODEROUTPUTVIEW",E_OUTOFMEMORY},
    {"CreateCryptoSession","PFND3D11_1DDI_CREATECRYPTOSESSION",E_OUTOFMEMORY},
    {"NegotiateCryptoSessionKeyExchange","PFND3D11_1DDI_NEGOTIATECRYPTOSESSIONKEYESCHANGE",E_INVALIDARG},
    {"CryptoSessionGetHandle","PFND3D11_1DDI_CRYPTOSESSIONGETHANDLE",E_OUTOFMEMORY},
    {"GetDataForNewHardwareKey","PFND3DWDDM2_0DDI_GETDATAFORNEWHARDWAREKEY",E_OUTOFMEMORY},
    {"CreateAuthenticatedChannel","PFND3D11_1DDI_CREATEAUTHENTICATEDCHANNEL",E_OUTOFMEMORY},
    {"NegotiateAuthenticatedChannelKeyExchange","PFND3D11_1DDI_NEGOTIATEAUTHENTICATEDCHANNELKEYEXCHANGE",E_INVALIDARG},
    {"QueryAuthenticatedChannel","PFND3D11_1DDI_QUERYAUTHENTICATEDCHANNEL",E_INVALIDARG},
    {"ConfigureAuthenticatedChannel","PFND3D11_1DDI_CONFIGUREAUTHENTICATEDCHANNEL",E_INVALIDARG},
    {"VideoProcessorSetOutputExtension","PFND3D11_1DDI_VIDEOPROCESSORSETOUTPUTEXTENSION",E_FAIL},
    {"VideoProcessorGetOutputExtension","PFND3D11_1DDI_VIDEOPROCESSORGETOUTPUTEXTENSION",E_FAIL},
    {"VideoProcessorSetStreamExtension","PFND3D11_1DDI_VIDEOPROCESSORSETSTREAMEXTENSION",E_FAIL},
    {"VideoProcessorGetStreamExtension","PFND3D11_1DDI_VIDEOPROCESSORGETSTREAMEXTENSION",E_FAIL},
};
const unsigned kVideoRefusalCount=unsigned(sizeof(kVideoRefusals)/sizeof(kVideoRefusals[0]));
namespace {
HRESULT refusal(const char *entry) {
    for (const auto &r:kVideoRefusals) {
        const char *a=r.entry,*b=entry;
        while (*a && *a==*b) { ++a; ++b; }
        if (!*a && !*b) return r.status;
    }
    return E_FAIL;
}
// The engine's video interfaces of this device, for one entry. A reference each, released at the end of the entry.
struct VideoDevice {
    ID3D11VideoDevice *p=nullptr;
    explicit VideoDevice(DeviceOwner &owner) {
        if (owner.device()) owner.device()->QueryInterface(__uuidof(ID3D11VideoDevice),reinterpret_cast<void **>(&p));
    }
    ~VideoDevice() { if (p) p->Release(); }
    VideoDevice(const VideoDevice &)=delete;
    VideoDevice &operator=(const VideoDevice &)=delete;
};
struct VideoContext {
    ID3D11VideoContext *p=nullptr;
    explicit VideoContext(ID3D11DeviceContext4 &context) {
        context.QueryInterface(__uuidof(ID3D11VideoContext),reinterpret_cast<void **>(&p));
    }
    ~VideoContext() { if (p) p->Release(); }
    VideoContext(const VideoContext &)=delete;
    VideoContext &operator=(const VideoContext &)=delete;
};
// An HRESULT entry. `failure` is the status this entry's page names for a failure the entry cannot describe; the
// call returns its own status, which passes through. A lost device and an exception give `failure` too: each of
// these entries' pages names what it may return, and that is the status the runtime gets. Every status other than
// S_OK writes one line (ddi-status-note.h).
template<typename F> HRESULT video_entry(D3D10DDI_HDEVICE h,const char *name,HRESULT failure,F &&call) noexcept {
    auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if (!storage || !storage->owner) return ddi_status_note(name,failure,DdiInputs("device=0"));
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    DdiEntryClassScope entryClass(DdiErrorClass::removed_only);
    if (!owner.device() || !owner.context()) return ddi_status_note(name,failure,DdiInputs("engine=lost"));
    HRESULT hr=failure;
    try { hr=call(owner,*owner.context()); }
    catch (...) { hr=failure; }
    return ddi_status_note(name,hr);
}
std::atomic_uint video_space_notes{8};
void note_space(const char *entry,D3DDDI_COLOR_SPACE_TYPE space) {
    unsigned count=video_space_notes.load(std::memory_order_relaxed);
    while (count && !video_space_notes.compare_exchange_weak(count,count-1,std::memory_order_relaxed)) {}
    if (!count) return;
    char line[192];
    std::snprintf(line,sizeof(line),"BC250 video: %s colour space %u is not admitted; the nearest BT.709 word is used\n",
        entry,unsigned(space));
    OutputDebugStringA(line);
    amdgpu_wddm_log::print("%s",line);
}
std::atomic_uint video_subobject_notes{8};

// ---- The decoder: none. The answers say so before any create.
void APIENTRY decoder_profile_count(D3D10DDI_HDEVICE,UINT *count) { if (count) *count=0; }
void APIENTRY decoder_profile(D3D10DDI_HDEVICE,UINT,GUID *profile) { if (profile) *profile=GUID{}; }
void APIENTRY check_decoder_format(D3D10DDI_HDEVICE,const GUID *,DXGI_FORMAT,BOOL *supported) { if (supported) *supported=FALSE; }
void APIENTRY decoder_config_count(D3D10DDI_HDEVICE,const D3D11_1DDI_VIDEO_DECODER_DESC *,UINT *count) { if (count) *count=0; }
void APIENTRY decoder_config(D3D10DDI_HDEVICE,const D3D11_1DDI_VIDEO_DECODER_DESC *,UINT,D3D11_1DDI_VIDEO_DECODER_CONFIG *config) {
    if (config) *config=D3D11_1DDI_VIDEO_DECODER_CONFIG{};
}
void APIENTRY decoder_buffer_type_count(D3D10DDI_HDEVICE,const D3D11_1DDI_VIDEO_DECODER_DESC *,UINT *count) { if (count) *count=0; }
void APIENTRY decoder_buffer_info(D3D10DDI_HDEVICE,const D3D11_1DDI_VIDEO_DECODER_DESC *,UINT,D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO *info) {
    if (info) *info=D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO{};
}
SIZE_T APIENTRY decoder_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODER *) { return sizeof(void *); }
HRESULT APIENTRY decoder_create(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODER *,D3D11_1DDI_HDECODE,D3D11_1DDI_HRTDECODE) {
    return ddi_status_note("CreateVideoDecoder",refusal("CreateVideoDecoder"),DdiInputs("profiles=0"));
}
void APIENTRY decoder_destroy(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE) {}
HRESULT APIENTRY decoder_extension(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE,const D3D11_1DDIARG_VIDEODECODEREXTENSION *) {
    return ddi_status_note("VideoDecoderExtension",refusal("VideoDecoderExtension"),DdiInputs("profiles=0"));
}
HRESULT APIENTRY decoder_begin(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE,const D3D11_1DDIARG_VIDEODECODERBEGINFRAME *) {
    return ddi_status_note("VideoDecoderBeginFrame",refusal("VideoDecoderBeginFrame"),DdiInputs("profiles=0"));
}
void APIENTRY decoder_end(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE) {}
HRESULT APIENTRY decoder_submit(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE,UINT,const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *) {
    return ddi_status_note("VideoDecoderSubmitBuffers",refusal("VideoDecoderSubmitBuffers"),DdiInputs("profiles=0"));
}
HRESULT APIENTRY decoder_submit1(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE,UINT,const D3DWDDM2_0DDI_VIDEO_DECODER_BUFFER_DESC1 *) {
    return ddi_status_note("VideoDecoderSubmitBuffers1",refusal("VideoDecoderSubmitBuffers1"),DdiInputs("profiles=0"));
}
HRESULT APIENTRY decoder_handle(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE,HANDLE *handle) {
    if (handle) *handle=nullptr;
    return ddi_status_note("VideoDecoderGetHandle",refusal("VideoDecoderGetHandle"),DdiInputs("profiles=0"));
}
HRESULT APIENTRY decoder_enable_downsampling(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE,D3DDDI_COLOR_SPACE_TYPE,
    const D3D11_1DDI_VIDEO_DECODER_DESC *,D3DDDI_COLOR_SPACE_TYPE,UINT) {
    return ddi_status_note("VideoDecoderEnableDownsampling",refusal("VideoDecoderEnableDownsampling"),DdiInputs("profiles=0"));
}
HRESULT APIENTRY decoder_update_downsampling(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE,const D3D11_1DDI_VIDEO_DECODER_DESC *,
    D3DDDI_COLOR_SPACE_TYPE) {
    return ddi_status_note("VideoDecoderUpdateDownsampling",refusal("VideoDecoderUpdateDownsampling"),DdiInputs("profiles=0"));
}
SIZE_T APIENTRY decoder_view_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *) { return sizeof(void *); }
HRESULT APIENTRY decoder_view_create(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *,
    D3D11_1DDI_HVIDEODECODEROUTPUTVIEW,D3D11_1DDI_HRTVIDEODECODEROUTPUTVIEW) {
    return ddi_status_note("CreateVideoDecoderOutputView",refusal("CreateVideoDecoderOutputView"),DdiInputs("profiles=0"));
}
void APIENTRY decoder_view_destroy(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEODECODEROUTPUTVIEW) {}
// The capability queries of WDDM 2.0 are all decoder queries (downsampling, decoder caps, histogram). The answer is
// the empty one: zero bytes of every kind.
void APIENTRY query_video_capabilities(D3D10DDI_HDEVICE,D3DWDDM2_0DDI_VIDEO_CAPABILITY_QUERY,UINT size,VOID *data) {
    if (data && size) std::memset(data,0,size);
}

// ---- Content protection: none. No crypto type, no key exchange, no certificate, no channel.
HRESULT APIENTRY content_protection_caps(D3D10DDI_HDEVICE,const GUID *,const GUID *,D3D11_1DDI_VIDEO_CONTENT_PROTECTION_CAPS *caps) {
    if (caps) *caps=D3D11_1DDI_VIDEO_CONTENT_PROTECTION_CAPS{};
    return S_OK;
}
HRESULT APIENTRY crypto_key_exchange_type(D3D10DDI_HDEVICE,const GUID *,const GUID *,UINT,GUID *type) {
    // The count is 0, so the runtime has no index to ask for. The answer for one is an empty GUID.
    if (type) *type=GUID{};
    return S_OK;
}
SIZE_T APIENTRY crypto_session_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATECRYPTOSESSION *) { return sizeof(void *); }
HRESULT APIENTRY crypto_session_create(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATECRYPTOSESSION *,
    D3D11_1DDI_HCRYPTOSESSION,D3D11_1DDI_HRTCRYPTOSESSION) {
    return ddi_status_note("CreateCryptoSession",refusal("CreateCryptoSession"),DdiInputs("crypto-types=0"));
}
void APIENTRY crypto_session_destroy(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION) {}
void APIENTRY certificate_size(D3D10DDI_HDEVICE,const D3D11_1DDI_CERTIFICATE_INFO *,UINT *size) { if (size) *size=0; }
void APIENTRY certificate(D3D10DDI_HDEVICE,const D3D11_1DDI_CERTIFICATE_INFO *,UINT size,BYTE *data) {
    if (data && size) std::memset(data,0,size);
}
HRESULT APIENTRY crypto_key_exchange(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,UINT,BYTE *) {
    return ddi_status_note("NegotiateCryptoSessionKeyExchange",refusal("NegotiateCryptoSessionKeyExchange"),DdiInputs("sessions=0"));
}
void APIENTRY encryption_blt(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,D3D10DDI_HRESOURCE,D3D10DDI_HRESOURCE,UINT,const VOID *) {}
void APIENTRY decryption_blt(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,D3D10DDI_HRESOURCE,D3D10DDI_HRESOURCE,
    const D3D11_1DDI_ENCRYPTED_BLOCK_INFO *,UINT,const VOID *,UINT,const VOID *) {}
void APIENTRY start_key_refresh(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,UINT size,VOID *data) {
    if (data && size) std::memset(data,0,size);
}
void APIENTRY finish_key_refresh(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION) {}
void APIENTRY encryption_blt_key(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,UINT size,VOID *key) {
    if (key && size) std::memset(key,0,size);
}
SIZE_T APIENTRY channel_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEAUTHENTICATEDCHANNEL *) { return sizeof(void *); }
HRESULT APIENTRY channel_create(D3D10DDI_HDEVICE,D3D11_1DDIARG_CREATEAUTHENTICATEDCHANNEL *,D3D11_1DDI_HAUTHCHANNEL,D3D11_1DDI_HRTAUTHCHANNEL) {
    return ddi_status_note("CreateAuthenticatedChannel",refusal("CreateAuthenticatedChannel"),DdiInputs("driver-channels=0"));
}
void APIENTRY channel_destroy(D3D10DDI_HDEVICE,D3D11_1DDI_HAUTHCHANNEL) {}
HRESULT APIENTRY channel_key_exchange(D3D10DDI_HDEVICE,D3D11_1DDI_HAUTHCHANNEL,UINT,VOID *) {
    return ddi_status_note("NegotiateAuthenticatedChannelKeyExchange",refusal("NegotiateAuthenticatedChannelKeyExchange"),DdiInputs("driver-channels=0"));
}
HRESULT APIENTRY channel_query(D3D10DDI_HDEVICE,D3D11_1DDI_HAUTHCHANNEL,UINT,const VOID *,UINT size,VOID *output) {
    if (output && size) std::memset(output,0,size);
    return ddi_status_note("QueryAuthenticatedChannel",refusal("QueryAuthenticatedChannel"),DdiInputs("driver-channels=0"));
}
HRESULT APIENTRY channel_configure(D3D10DDI_HDEVICE,D3D11_1DDI_HAUTHCHANNEL,UINT,const VOID *,D3D11_1DDI_AUTHENTICATED_CONFIGURE_OUTPUT *output) {
    if (output) *output=D3D11_1DDI_AUTHENTICATED_CONFIGURE_OUTPUT{};
    return ddi_status_note("ConfigureAuthenticatedChannel",refusal("ConfigureAuthenticatedChannel"),DdiInputs("driver-channels=0"));
}
HRESULT APIENTRY crypto_session_handle(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,HANDLE *handle) {
    if (handle) *handle=nullptr;
    return ddi_status_note("CryptoSessionGetHandle",refusal("CryptoSessionGetHandle"),DdiInputs("sessions=0"));
}
void APIENTRY capture_handle(D3D10DDI_HDEVICE,D3D11_1DDI_GETCAPTUREHANDLEDATA *data) {
    if (data) { data->hAllocation=0; data->DataOffset=0; data->DataSize=0; }
}
HRESULT APIENTRY new_hardware_key(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,UINT,const void *,UINT64 *output) {
    if (output) *output=0;
    return ddi_status_note("GetDataForNewHardwareKey",refusal("GetDataForNewHardwareKey"),DdiInputs("sessions=0"));
}
void APIENTRY crypto_session_status(D3D10DDI_HDEVICE,D3D11_1DDI_HCRYPTOSESSION,D3DWDDM2_0DDI_CRYPTO_SESSION_STATUS *status) {
    if (status) *status=D3DWDDM2_0DDI_CRYPTO_SESSION_STATUS_OK;
}
void APIENTRY crypto_session_private_size(D3D10DDI_HDEVICE,const GUID *,const GUID *,const GUID *,UINT *input,UINT *output) {
    if (input) *input=0;
    if (output) *output=0;
}

// ---- The video processor: enumerator, caps, processor, views, state and blit.
SIZE_T APIENTRY enum_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *) { return sizeof(DdiVideoProcessorEnum); }
// CreateVideoProcessorEnum: S_OK or E_OUTOFMEMORY.
HRESULT APIENTRY enum_create(D3D10DDI_HDEVICE h,const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *args,
    D3D11_1DDI_HVIDEOPROCESSORENUM handle,D3D11_1DDI_HRTVIDEOPROCESSORENUM) {
    auto *s=static_cast<DdiVideoProcessorEnum *>(handle.pDrvPrivate);
    if (s) { s->object=nullptr; s->formats={}; }
    return video_entry(h,"CreateVideoProcessorEnum",E_OUTOFMEMORY,[&](DeviceOwner &owner,ID3D11DeviceContext4 &) -> HRESULT {
        if (!s || !args || !video_content_admitted(args->Desc)) return E_OUTOFMEMORY;
        VideoDevice video(owner);
        if (!video.p) return E_OUTOFMEMORY;
        const auto desc=convert_video_content(args->Desc);
        ID3D11VideoProcessorEnumerator *object=nullptr;
        const HRESULT hr=video.p->CreateVideoProcessorEnumerator(&desc,&object);
        if (FAILED(hr) || !object) { if (object) object->Release(); return E_OUTOFMEMORY; }
        s->object=object;
        s->formats.fill([&](DXGI_FORMAT format,UINT *support) { return object->CheckVideoProcessorFormat(format,support); });
        return S_OK;
    });
}
void APIENTRY enum_destroy(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSORENUM handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiVideoProcessorEnum *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
// Format answers are cached at enumerator creation and survive device loss.
void APIENTRY check_processor_format(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM handle,DXGI_FORMAT format,UINT *support) {
    const auto *s=static_cast<const DdiVideoProcessorEnum *>(handle.pDrvPrivate);
    if (support) *support=s ? s->formats.get(format) : 0;
}
void APIENTRY processor_caps(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM,D3D11_1DDI_VIDEO_PROCESSOR_CAPS *caps) {
    if (caps) *caps=video_processor_caps();
}
void APIENTRY rate_conversion_caps(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM,UINT,
    D3D11_1DDI_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps) {
    if (caps) *caps=video_rate_conversion_caps();
}
void APIENTRY custom_rate(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM,UINT,UINT,D3D11_1DDI_VIDEO_PROCESSOR_CUSTOM_RATE *rate) {
    if (rate) *rate=D3D11_1DDI_VIDEO_PROCESSOR_CUSTOM_RATE{};
}
void APIENTRY filter_range(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM,D3D11_1DDI_VIDEO_PROCESSOR_FILTER,
    D3D11_1DDI_VIDEO_PROCESSOR_FILTER_RANGE *range) {
    // FilterCaps is 0, so no filter has a range. The empty range has a multiplier of 1, as the API documents a
    // range with no steps.
    if (range) *range=D3D11_1DDI_VIDEO_PROCESSOR_FILTER_RANGE{0,0,0,1.0f};
}
void APIENTRY check_format_conversion(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM handle,
    D3DWDDM2_0DDI_CHECK_VIDEO_PROCESSOR_FORMAT_CONVERSION *c) {
    if (!c) return;
    const auto *s=static_cast<const DdiVideoProcessorEnum *>(handle.pDrvPrivate);
    c->Supported=s && (s->formats.get(c->InputFormat)&D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) &&
        (s->formats.get(c->OutputFormat)&D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) &&
        video_conversion_supported(c->InputFormat,static_cast<DXGI_COLOR_SPACE_TYPE>(c->InputColorSpace),
        c->OutputFormat,static_cast<DXGI_COLOR_SPACE_TYPE>(c->OutputColorSpace)) ? TRUE : FALSE;
}
SIZE_T APIENTRY processor_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSOR *) { return sizeof(DdiVideoProcessor); }
// CreateVideoProcessor: S_OK, D3DDDIERR_DEVICEREMOVED or E_OUTOFMEMORY.
HRESULT APIENTRY processor_create(D3D10DDI_HDEVICE h,const D3D11_1DDIARG_CREATEVIDEOPROCESSOR *args,
    D3D11_1DDI_HVIDEOPROCESSOR handle,D3D11_1DDI_HRTVIDEOPROCESSOR) {
    auto *s=static_cast<DdiVideoProcessor *>(handle.pDrvPrivate);
    if (s) s->object=nullptr;
    return video_entry(h,"CreateVideoProcessor",E_OUTOFMEMORY,[&](DeviceOwner &owner,ID3D11DeviceContext4 &) -> HRESULT {
        auto *e=args ? static_cast<DdiVideoProcessorEnum *>(args->hVideoProcessorEnum.pDrvPrivate) : nullptr;
        if (!s || !e || !e->object || args->RateConversionCapsIndex>=video_processor_caps().RateConversionCapsCount)
            return E_OUTOFMEMORY;
        VideoDevice video(owner);
        if (!video.p) return E_OUTOFMEMORY;
        ID3D11VideoProcessor *object=nullptr;
        const HRESULT hr=video.p->CreateVideoProcessor(e->object,args->RateConversionCapsIndex,&object);
        if (FAILED(hr) || !object) { if (object) object->Release(); return ddi_device_status(hr)==D3DDDIERR_DEVICEREMOVED ? D3DDDIERR_DEVICEREMOVED : E_OUTOFMEMORY; }
        s->object=object;
        return S_OK;
    });
}
void APIENTRY processor_destroy(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiVideoProcessor *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
ID3D11VideoProcessor *processor_of(D3D11_1DDI_HVIDEOPROCESSOR handle) {
    auto *s=static_cast<DdiVideoProcessor *>(handle.pDrvPrivate);
    return s ? s->object : nullptr;
}
// The state setters of the processor that the engine keeps. Each forwards to the engine's video context. A setter
// whose state the engine does not keep (the alpha fill mode, constriction, stream output rate, alpha, palette, pixel
// aspect ratio, luma key, stereo format, filters and mirror) is accepted and kept nowhere: the caps claim none of
// those features, and the engine logs each such call as a stub.
template<typename F> void processor_state(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR handle,F &&set) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *processor=processor_of(handle);
        if (!processor) return; // The runtime checked the handle; a create that failed left no object.
        VideoContext video(context);
        if (video.p) set(*video.p,processor);
    });
}
void APIENTRY output_target_rect(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,BOOL enable,const RECT *rect) {
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) {
        v.VideoProcessorSetOutputTargetRect(o,enable && rect ? TRUE : FALSE,enable ? rect : nullptr);
    });
}
void APIENTRY output_background(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,BOOL ycbcr,const D3D11_1DDI_VIDEO_COLOR *color) {
    if (!color) return;
    const D3D11_VIDEO_COLOR c=convert_video_color(*color);
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) { v.VideoProcessorSetOutputBackgroundColor(o,ycbcr,&c); });
}
void APIENTRY output_space(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,const D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE *space) {
    if (!space) return;
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE s=convert_video_space(*space);
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) { v.VideoProcessorSetOutputColorSpace(o,&s); });
}
void APIENTRY output_alpha_fill(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,D3D11_1DDI_VIDEO_PROCESSOR_ALPHA_FILL_MODE,UINT) {}
void APIENTRY output_constriction(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,BOOL,SIZE) {}
void APIENTRY output_stereo(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,BOOL enable) {
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) { v.VideoProcessorSetOutputStereoMode(o,enable); });
}
// The extensions: no private extension of any vendor. A browser asks for AMD's HDR offload extension on every AMD
// adapter (Chromium GetGpuDriverOverlayInfo) and takes E_FAIL as "not there": an ordinary answer, so no line.
HRESULT APIENTRY output_extension_set(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,const GUID *,UINT,void *) {
    return refusal("VideoProcessorSetOutputExtension");
}
HRESULT APIENTRY output_extension_get(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,const GUID *,UINT size,void *data) {
    if (data && size) std::memset(data,0,size);
    return refusal("VideoProcessorGetOutputExtension");
}
void APIENTRY stream_frame_format(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,D3D11_1DDI_VIDEO_FRAME_FORMAT format) {
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) {
        v.VideoProcessorSetStreamFrameFormat(o,stream,static_cast<D3D11_VIDEO_FRAME_FORMAT>(format));
    });
}
void APIENTRY stream_space(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,const D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE *space) {
    if (!space) return;
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE s=convert_video_space(*space);
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) { v.VideoProcessorSetStreamColorSpace(o,stream,&s); });
}
void APIENTRY stream_output_rate(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,D3D11_1DDI_VIDEO_PROCESSOR_OUTPUT_RATE,BOOL,const DXGI_RATIONAL *) {}
void APIENTRY stream_source_rect(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,BOOL enable,const RECT *rect) {
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) {
        v.VideoProcessorSetStreamSourceRect(o,stream,enable && rect ? TRUE : FALSE,enable ? rect : nullptr);
    });
}
void APIENTRY stream_dest_rect(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,BOOL enable,const RECT *rect) {
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) {
        v.VideoProcessorSetStreamDestRect(o,stream,enable && rect ? TRUE : FALSE,enable ? rect : nullptr);
    });
}
void APIENTRY stream_alpha(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,BOOL,FLOAT) {}
void APIENTRY stream_palette(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,UINT,const UINT *) {}
void APIENTRY stream_aspect(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,BOOL,const DXGI_RATIONAL *,const DXGI_RATIONAL *) {}
void APIENTRY stream_luma_key(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,BOOL,FLOAT,FLOAT) {}
void APIENTRY stream_stereo(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,BOOL,D3D11_1DDI_VIDEO_PROCESSOR_STEREO_FORMAT,
    BOOL,BOOL,D3D11_1DDI_VIDEO_PROCESSOR_STEREO_FLIP_MODE,int) {}
void APIENTRY stream_auto_processing(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,BOOL enable) {
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) { v.VideoProcessorSetStreamAutoProcessingMode(o,stream,enable); });
}
void APIENTRY stream_filter(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,D3D11_1DDI_VIDEO_PROCESSOR_FILTER,BOOL,int) {}
void APIENTRY stream_rotation(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,BOOL enable,D3D11_1DDI_VIDEO_PROCESSOR_ROTATION rotation) {
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) {
        v.VideoProcessorSetStreamRotation(o,stream,enable,static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(rotation));
    });
}
HRESULT APIENTRY stream_extension_set(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,const GUID *,UINT,void *) {
    return refusal("VideoProcessorSetStreamExtension");
}
HRESULT APIENTRY stream_extension_get(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,const GUID *,UINT size,void *data) {
    if (data && size) std::memset(data,0,size);
    return refusal("VideoProcessorGetStreamExtension");
}
// WDDM 2.0: the DXGI colour spaces. The engine takes the legacy word, so a space becomes the word of
// ddi-video-caps.h; a space that the processor does not convert (P2020, PQ, HLG, scRGB) gets the nearest BT.709
// word and one line. CheckVideoProcessorFormatConversion says no to it first, so an application that asks gets the
// truth.
void APIENTRY output_space1(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,D3DDDI_COLOR_SPACE_TYPE space) {
    const auto mapped=video_color_space(static_cast<DXGI_COLOR_SPACE_TYPE>(space));
    if (!mapped.admitted) note_space("VideoProcessorSetOutputColorSpace1",space);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE s=convert_video_space(mapped.legacy);
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) { v.VideoProcessorSetOutputColorSpace(o,&s); });
}
void APIENTRY stream_space1(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,D3DDDI_COLOR_SPACE_TYPE space) {
    const auto mapped=video_color_space(static_cast<DXGI_COLOR_SPACE_TYPE>(space));
    if (!mapped.admitted) note_space("VideoProcessorSetStreamColorSpace1",space);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE s=convert_video_space(mapped.legacy);
    processor_state(h,p,[&](ID3D11VideoContext &v,ID3D11VideoProcessor *o) { v.VideoProcessorSetStreamColorSpace(o,stream,&s); });
}
void APIENTRY output_shader_usage(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,BOOL) {}
void APIENTRY stream_mirror(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,BOOL,BOOL,BOOL) {}
// GetBehaviorHints: no hint. The hints ask the compositor to keep rotation, resizing or colour conversion out of a
// multiplane overlay; the processor here does all three in the blit, so it asks for none.
HRESULT APIENTRY behavior_hints(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,UINT,DXGI_FORMAT,UINT,
    const D3DWDDM2_0DDI_VIDEO_PROCESSOR_STREAM_BEHAVIOR_HINT *,UINT *hints) {
    if (hints) *hints=0;
    return S_OK;
}
// WDDM 2.1: HDR metadata. The processor does no tone mapping (FeatureCaps has no METADATA_HDR10), so the metadata
// changes nothing.
void APIENTRY output_hdr_metadata(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,D3DDDI_HDR_METADATA_TYPE,UINT,const VOID *) {}
void APIENTRY stream_hdr_metadata(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR,UINT,D3DDDI_HDR_METADATA_TYPE,UINT,const VOID *) {}

// The views. The format decides: a format outside kVideoFormats in its direction is DXGI_ERROR_UNSUPPORTED, the
// status the two create pages name for a view the driver cannot make.
bool texture_format(const DdiResource *r,DXGI_FORMAT &format) {
    if (!r || !r->object) return false;
    D3D11_RESOURCE_DIMENSION dimension{};
    r->object->GetType(&dimension);
    if (dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D *>(r->object)->GetDesc(&d);
    format=d.Format;
    return true;
}
SIZE_T APIENTRY input_view_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW *) { return sizeof(DdiVideoProcessorInputView); }
HRESULT APIENTRY input_view_create(D3D10DDI_HDEVICE h,const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW *args,
    D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW handle,D3D11_1DDI_HRTVIDEOPROCESSORINPUTVIEW) {
    auto *s=static_cast<DdiVideoProcessorInputView *>(handle.pDrvPrivate);
    if (s) s->object=nullptr;
    return video_entry(h,"CreateVideoProcessorInputView",E_OUTOFMEMORY,[&](DeviceOwner &owner,ID3D11DeviceContext4 &) -> HRESULT {
        auto *r=args ? static_cast<DdiResource *>(args->hDrvResource.pDrvPrivate) : nullptr;
        auto *e=args ? static_cast<DdiVideoProcessorEnum *>(args->hDrvVideoProcessorEnum.pDrvPrivate) : nullptr;
        if (!s || !e || !e->object) return E_OUTOFMEMORY;
        DXGI_FORMAT format{};
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        if (!texture_format(r,format) || !plan_video_input_view(*args,format,desc)) return DXGI_ERROR_UNSUPPORTED;
        VideoDevice video(owner);
        if (!video.p) return E_OUTOFMEMORY;
        ID3D11VideoProcessorInputView *object=nullptr;
        const HRESULT hr=video.p->CreateVideoProcessorInputView(r->object,e->object,&desc,&object);
        if (FAILED(hr) || !object) {
            if (object) object->Release();
            return ddi_device_status(hr)==D3DDDIERR_DEVICEREMOVED ? D3DDDIERR_DEVICEREMOVED : E_OUTOFMEMORY;
        }
        s->object=object;
        return S_OK;
    });
}
void APIENTRY input_view_destroy(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiVideoProcessorInputView *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
SIZE_T APIENTRY output_view_size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW *) { return sizeof(DdiVideoProcessorOutputView); }
HRESULT APIENTRY output_view_create(D3D10DDI_HDEVICE h,const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW *args,
    D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW handle,D3D11_1DDI_HRTVIDEOPROCESSOROUTPUTVIEW) {
    auto *s=static_cast<DdiVideoProcessorOutputView *>(handle.pDrvPrivate);
    if (s) s->object=nullptr;
    return video_entry(h,"CreateVideoProcessorOutputView",E_OUTOFMEMORY,[&](DeviceOwner &owner,ID3D11DeviceContext4 &) -> HRESULT {
        auto *r=args ? static_cast<DdiResource *>(args->hDrvResource.pDrvPrivate) : nullptr;
        auto *e=args ? static_cast<DdiVideoProcessorEnum *>(args->hDrvVideoProcessorEnum.pDrvPrivate) : nullptr;
        if (!s || !e || !e->object) return E_OUTOFMEMORY;
        DXGI_FORMAT format{};
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
        if (!texture_format(r,format) || !plan_video_output_view(*args,format,desc)) return DXGI_ERROR_UNSUPPORTED;
        VideoDevice video(owner);
        if (!video.p) return E_OUTOFMEMORY;
        ID3D11VideoProcessorOutputView *object=nullptr;
        const HRESULT hr=video.p->CreateVideoProcessorOutputView(r->object,e->object,&desc,&object);
        if (FAILED(hr) || !object) {
            if (object) object->Release();
            return ddi_device_status(hr)==D3DDDIERR_DEVICEREMOVED ? D3DDDIERR_DEVICEREMOVED : E_OUTOFMEMORY;
        }
        s->object=object;
        return S_OK;
    });
}
void APIENTRY output_view_destroy(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiVideoProcessorOutputView *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
// The engine tracks every read after a write of its own resources, so the hazard needs no barrier from here.
void APIENTRY input_view_hazard(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW,D3D10DDI_HRESOURCE) {}
ID3D11VideoProcessorInputView *input_of(D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW handle) {
    auto *s=static_cast<DdiVideoProcessorInputView *>(handle.pDrvPrivate);
    return s ? s->object : nullptr;
}
// VideoProcessorBlt: S_OK or E_OUTOFMEMORY. The processor has no past or future frames (RateConversionCaps 0) and no
// stereo, so those surfaces stop here; the engine would ignore them and log each.
HRESULT APIENTRY blt(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW out,UINT frame,
    UINT count,const D3D11_1DDI_VIDEO_PROCESSOR_STREAM *streams) {
    return video_entry(h,"VideoProcessorBlt",E_OUTOFMEMORY,[&](DeviceOwner &,ID3D11DeviceContext4 &context) -> HRESULT {
        auto *processor=processor_of(p);
        auto *o=static_cast<DdiVideoProcessorOutputView *>(out.pDrvPrivate);
        if (!processor || !o || !o->object || (count && !streams)) return E_OUTOFMEMORY;
        std::vector<D3D11_VIDEO_PROCESSOR_STREAM> engine(count);
        for (UINT i=0;i<count;++i) {
            auto &d=engine[i];
            const auto &s=streams[i];
            d.Enable=s.Enable;
            d.OutputIndex=s.OutputIndex;
            d.InputFrameOrField=s.InputFrameOrField;
            d.pInputSurface=input_of(s.hInputSurface);
            if (d.Enable && !d.pInputSurface) return E_OUTOFMEMORY;
        }
        VideoContext video(context);
        if (!video.p) return E_OUTOFMEMORY;
        return SUCCEEDED(video.p->VideoProcessorBlt(processor,o->object,frame,count,engine.data())) ? S_OK : E_OUTOFMEMORY;
    });
}
}

D3DWDDM2_1DDI_VIDEODEVICEFUNCS make_video_device_table() {
    D3DWDDM2_1DDI_VIDEODEVICEFUNCS t{};
    t.pfnGetVideoDecoderProfileCount=decoder_profile_count;
    t.pfnGetVideoDecoderProfile=decoder_profile;
    t.pfnCheckVideoDecoderFormat=check_decoder_format;
    t.pfnGetVideoDecoderConfigCount=decoder_config_count;
    t.pfnGetVideoDecoderConfig=decoder_config;
    t.pfnGetVideoDecoderBufferTypeCount=decoder_buffer_type_count;
    t.pfnGetVideoDecoderBufferInfo=decoder_buffer_info;
    t.pfnCalcPrivateVideoDecoderSize=decoder_size;
    t.pfnCreateVideoDecoder=decoder_create;
    t.pfnDestroyVideoDecoder=decoder_destroy;
    t.pfnVideoDecoderExtension=decoder_extension;
    t.pfnVideoDecoderBeginFrame=decoder_begin;
    t.pfnVideoDecoderEndFrame=decoder_end;
    t.pfnVideoDecoderSubmitBuffers=decoder_submit;
    t.pfnCalcPrivateVideoProcessorEnumSize=enum_size;
    t.pfnCreateVideoProcessorEnum=enum_create;
    t.pfnDestroyVideoProcessorEnum=enum_destroy;
    t.pfnCheckVideoProcessorFormat=check_processor_format;
    t.pfnGetVideoProcessorCaps=processor_caps;
    t.pfnGetVideoProcessorRateConversionCaps=rate_conversion_caps;
    t.pfnGetVideoProcessorCustomRate=custom_rate;
    t.pfnGetVideoProcessorFilterRange=filter_range;
    t.pfnCalcPrivateVideoProcessorSize=processor_size;
    t.pfnCreateVideoProcessor=processor_create;
    t.pfnDestroyVideoProcessor=processor_destroy;
    t.pfnVideoProcessorSetOutputTargetRect=output_target_rect;
    t.pfnVideoProcessorSetOutputBackgroundColor=output_background;
    t.pfnVideoProcessorSetOutputColorSpace=output_space;
    t.pfnVideoProcessorSetOutputAlphaFillMode=output_alpha_fill;
    t.pfnVideoProcessorSetOutputConstriction=output_constriction;
    t.pfnVideoProcessorSetOutputStereoMode=output_stereo;
    t.pfnVideoProcessorSetOutputExtension=output_extension_set;
    t.pfnVideoProcessorGetOutputExtension=output_extension_get;
    t.pfnVideoProcessorSetStreamFrameFormat=stream_frame_format;
    t.pfnVideoProcessorSetStreamColorSpace=stream_space;
    t.pfnVideoProcessorSetStreamOutputRate=stream_output_rate;
    t.pfnVideoProcessorSetStreamSourceRect=stream_source_rect;
    t.pfnVideoProcessorSetStreamDestRect=stream_dest_rect;
    t.pfnVideoProcessorSetStreamAlpha=stream_alpha;
    t.pfnVideoProcessorSetStreamPalette=stream_palette;
    t.pfnVideoProcessorSetStreamPixelAspectRatio=stream_aspect;
    t.pfnVideoProcessorSetStreamLumaKey=stream_luma_key;
    t.pfnVideoProcessorSetStreamStereoFormat=stream_stereo;
    t.pfnVideoProcessorSetStreamAutoProcessingMode=stream_auto_processing;
    t.pfnVideoProcessorSetStreamFilter=stream_filter;
    t.pfnVideoProcessorSetStreamExtension=stream_extension_set;
    t.pfnVideoProcessorGetStreamExtension=stream_extension_get;
    t.pfnVideoProcessorBlt=blt;
    t.pfnCalcPrivateVideoDecoderOutputViewSize=decoder_view_size;
    t.pfnCreateVideoDecoderOutputView=decoder_view_create;
    t.pfnDestroyVideoDecoderOutputView=decoder_view_destroy;
    t.pfnCalcPrivateVideoProcessorInputViewSize=input_view_size;
    t.pfnCreateVideoProcessorInputView=input_view_create;
    t.pfnDestroyVideoProcessorInputView=input_view_destroy;
    t.pfnCalcPrivateVideoProcessorOutputViewSize=output_view_size;
    t.pfnCreateVideoProcessorOutputView=output_view_create;
    t.pfnDestroyVideoProcessorOutputView=output_view_destroy;
    t.pfnVideoProcessorInputViewReadAfterWriteHazard=input_view_hazard;
    t.pfnGetContentProtectionCaps=content_protection_caps;
    t.pfnGetCryptoKeyExchangeType=crypto_key_exchange_type;
    t.pfnCalcPrivateCryptoSessionSize=crypto_session_size;
    t.pfnCreateCryptoSession=crypto_session_create;
    t.pfnDestroyCryptoSession=crypto_session_destroy;
    t.pfnGetCertificateSize=certificate_size;
    t.pfnGetCertificate=certificate;
    t.pfnNegotiateCryptoSessionKeyExchange=crypto_key_exchange;
    t.pfnEncryptionBlt=encryption_blt;
    t.pfnDecryptionBlt=decryption_blt;
    t.pfnStartSessionKeyRefresh=start_key_refresh;
    t.pfnFinishSessionKeyRefresh=finish_key_refresh;
    t.pfnGetEncryptionBltKey=encryption_blt_key;
    t.pfnCalcPrivateAuthenticatedChannelSize=channel_size;
    t.pfnCreateAuthenticatedChannel=channel_create;
    t.pfnDestroyAuthenticatedChannel=channel_destroy;
    t.pfnNegotiateAuthenticatedChannelKeyExchange=channel_key_exchange;
    t.pfnQueryAuthenticatedChannel=channel_query;
    t.pfnConfigureAuthenticatedChannel=channel_configure;
    t.pfnVideoDecoderGetHandle=decoder_handle;
    t.pfnCryptoSessionGetHandle=crypto_session_handle;
    t.pfnVideoProcessorSetStreamRotation=stream_rotation;
    t.pfnGetCaptureHandle=capture_handle;
    t.pfnGetDataForNewHardwareKey=new_hardware_key;
    t.pfnCheckCryptoSessionStatus=crypto_session_status;
    t.pfnVideoDecoderSubmitBuffers1=decoder_submit1;
    t.pfnQueryVideoCapabilities=query_video_capabilities;
    t.pfnCheckVideoProcessorFormatConversion=check_format_conversion;
    t.pfnVideoDecoderEnableDownsampling=decoder_enable_downsampling;
    t.pfnVideoDecoderUpdateDownsampling=decoder_update_downsampling;
    t.pfnVideoProcessorSetStreamMirror=stream_mirror;
    t.pfnVideoProcessorSetOutputColorSpace1=output_space1;
    t.pfnVideoProcessorSetStreamColorSpace1=stream_space1;
    t.pfnVideoProcessorSetOutputShaderUsage=output_shader_usage;
    t.pfnVideoProcessorGetBehaviorHints=behavior_hints;
    t.pfnGetCryptoSessionPrivateDataSize=crypto_session_private_size;
    t.pfnVideoProcessorSetOutputHDRMetaData=output_hdr_metadata;
    t.pfnVideoProcessorSetStreamHDRMetaData=stream_hdr_metadata;
    return t;
}
unsigned video_subobject_notes_left() { return video_subobject_notes.load(std::memory_order_relaxed); }
// RetrieveSubObject. The page names "S_OK or an appropriate error"; the function is free-threaded and holds no
// state of the device: the table is the same for every device, and the objects it creates belong to the device.
// One line for each call, in both flavours, at most eight in a process: lab arm A reads which sub ID the runtime asks
// for at our interface.
HRESULT APIENTRY retrieve_subobject(D3D10DDI_HDEVICE,UINT32 subId,SIZE_T paramSize,void *params,
    SIZE_T outputSize,void *output) {
    const SIZE_T size=video_table_size(subId);
    HRESULT hr=S_OK;
    BOOL relocate=FALSE;
    if (!size) hr=E_NOTIMPL;
    else if (!params || paramSize<sizeof(D3D11_1DDI_VIDEO_INPUT)) hr=E_INVALIDARG;
    else {
        const auto *input=static_cast<const D3D11_1DDI_VIDEO_INPUT *>(params);
        relocate=input->Relocate;
        void *table=input->p11VideoDeviceFuncs;
        if (!table) hr=E_INVALIDARG;
        else {
            const D3DWDDM2_1DDI_VIDEODEVICEFUNCS all=make_video_device_table();
            std::memcpy(table,&all,size);
            if (output && outputSize) std::memset(output,0,outputSize);
        }
    }
    unsigned count=video_subobject_notes.load(std::memory_order_relaxed);
    while (count && !video_subobject_notes.compare_exchange_weak(count,count-1,std::memory_order_relaxed)) {}
    if (count) {
        char line[224];
        std::snprintf(line,sizeof(line),"BC250 video: RetrieveSubObject sub=%u param=%zu output=%zu relocate=%u "
            "table-bytes=%zu hr=%08X\n",unsigned(subId),size_t(paramSize),size_t(outputSize),unsigned(relocate),
            size_t(size),unsigned(hr));
        OutputDebugStringA(line);
        amdgpu_wddm_log::print("%s",line);
    }
    return hr;
}
}
