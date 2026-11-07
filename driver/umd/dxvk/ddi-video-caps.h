// SPDX-License-Identifier: MIT
#pragma once
// The answers of the video processor of this shell: the formats it reads and writes, the colour spaces it converts,
// and its caps. Pure functions, no engine: ddi-video.cpp answers the runtime from them, ddi-format.cpp sets the
// video bits of CheckFormatSupport from the same table, and ddi-video-test.cpp checks them on the host.
//
// The processor is the engine's (DXVK d3d11_video.cpp): one full-screen draw per enabled stream, progressive frames,
// bilinear scaling inside the source rectangle, and one colour transform from the input colour space to the output
// colour space. It has no deinterlacing, no frame rate conversion, no filters, no stereo, no rotation and no HDR
// tone mapping, so the caps below claim none of them.
#include <d3d10umddi.h>
#include <d3d11.h>
#include <dxgicommon.h>
#include <dxgiformat.h>
namespace bc250::umd {
struct VideoFormatRow {
    DXGI_FORMAT format;
    bool input;   // a video processor input view may read it
    bool output;  // a video processor output view may write it
    bool ycbcr;   // the data is YCbCr (the colour space must be a YCbCr one)
};
// NV12 out is the YUV swap chain of a browser (DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO): the engine writes Y and CbCr into
// the two planes. P010 uses 16-bit plane views; YUY2 uses an RGBA8 packed-pair image.
// This table is a policy ceiling, intersected with the engine's directional query.
// AYUV, Y210, Y410 and P016 are not in the table: no host or lab check of them exists.
inline constexpr VideoFormatRow kVideoFormats[]={
    {DXGI_FORMAT_NV12,true,true,true},
    {DXGI_FORMAT_P010,true,true,true},
    {DXGI_FORMAT_YUY2,true,true,true},
    {DXGI_FORMAT_B8G8R8A8_UNORM,true,true,false},
    {DXGI_FORMAT_R8G8B8A8_UNORM,true,true,false},
    {DXGI_FORMAT_R10G10B10A2_UNORM,true,true,false},
};
constexpr const VideoFormatRow *video_format_row(DXGI_FORMAT format) {
    for (const auto &row:kVideoFormats) if (row.format==format) return &row;
    return nullptr;
}
// CheckVideoProcessorFormat: D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT/OUTPUT.
constexpr UINT video_processor_format_support(DXGI_FORMAT format) {
    const auto *row=video_format_row(format);
    if (!row) return 0;
    return (row->input ? UINT(D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) : 0u) |
           (row->output ? UINT(D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) : 0u);
}
// The CheckFormatSupport bits of the same table. A format that the engine does not support at all gets none, so
// the caller passes the engine's answer for the format.
constexpr UINT video_format_support_bits(DXGI_FORMAT format,bool engineSupportsFormat) {
    const auto *row=engineSupportsFormat ? video_format_row(format) : nullptr;
    if (!row) return 0;
    return (row->input ? UINT(D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) : 0u) |
           (row->output ? UINT(D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) : 0u);
}
// Format capabilities belong to the exact engine/device pair. Cache the
// enumerator's query while creation succeeds, so read-only DDI queries retain
// the measured directions after device loss without calling the lost engine.
struct VideoFormatSupport {
    UINT values[sizeof(kVideoFormats)/sizeof(kVideoFormats[0])]{};
    template<typename Query> void fill(Query query) {
        for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) {
            UINT support=0;
            values[i]=SUCCEEDED(query(kVideoFormats[i].format,&support))
                ? support & video_processor_format_support(kVideoFormats[i].format) : 0;
        }
    }
    UINT get(DXGI_FORMAT format) const {
        for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++)
            if(kVideoFormats[i].format==format) return values[i];
        return 0;
    }
};
constexpr UINT video_format_support_bits_masked(DXGI_FORMAT format,UINT engine) {
    UINT bits=0;
    if(engine&D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) bits|=D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT;
    if(engine&D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) bits|=D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
    return bits & video_format_support_bits(format,true);
}
// A colour space of ColorSpace1, as the engine's legacy colour-space word (D3D11_VIDEO_PROCESSOR_COLOR_SPACE) states
// it. The legacy word has no primaries and no transfer function: the engine converts the matrix and the range only
// and never linearizes. So an admitted space is one whose primaries are BT.709 (BT.601 matrices with 709 primaries
// included, as video on the web is), with a G22 or G24 (BT.1886) SDR transfer, which both pass through unchanged.
// P2020, G2084 (PQ), HLG and G10 (scRGB) need a gamut or a tone mapping that this processor does not do: they are
// not admitted. For one of them `legacy` is the nearest word (the BT.709 matrix at its range), the conversion check
// says no, and the blit is a deviation that the README states.
struct VideoColorSpace {
    bool admitted=false;
    bool ycbcr=false;
    D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE legacy{};
};
constexpr D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE video_legacy_space(bool rgbStudio,bool bt709,
    D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE range) {
    D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE s{};
    s.Usage=0;                       // playback
    s.RGB_Range=rgbStudio ? 1u : 0u; // 0: 0-255, 1: 16-235
    s.YCbCr_Matrix=bt709 ? 1u : 0u;  // 0: BT.601, 1: BT.709
    s.YCbCr_xvYCC=0;
    s.Nominal_Range=UINT(range);
    return s;
}
constexpr VideoColorSpace video_color_space(DXGI_COLOR_SPACE_TYPE space) {
    using R=D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE;
    constexpr R studio=D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    constexpr R full=D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    switch (space) {
    // RGB. The range word of an RGB space is RGB_Range; Nominal_Range stays the full range of the samples.
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return {true,false,video_legacy_space(false,true,full)};
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709: return {true,false,video_legacy_space(true,true,studio)};
    // YCbCr with BT.709 primaries.
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601: return {true,true,video_legacy_space(false,false,full)};
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601: return {true,true,video_legacy_space(false,false,studio)};
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601: return {true,true,video_legacy_space(false,false,full)};
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709: return {true,true,video_legacy_space(false,true,studio)};
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709: return {true,true,video_legacy_space(false,true,full)};
    // Not admitted: the nearest legacy word.
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020: return {false,false,video_legacy_space(false,true,full)};
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020: return {false,false,video_legacy_space(true,true,studio)};
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020: return {false,true,video_legacy_space(false,true,full)};
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020: return {false,true,video_legacy_space(false,true,studio)};
    default: return {false,false,video_legacy_space(false,true,full)};
    }
}
// CheckVideoProcessorFormatConversion: both formats in the table in their direction, both colour spaces admitted,
// and each space of the kind of its format (a YCbCr space on a YCbCr format, an RGB space on an RGB format).
constexpr bool video_conversion_supported(DXGI_FORMAT inFormat,DXGI_COLOR_SPACE_TYPE inSpace,
    DXGI_FORMAT outFormat,DXGI_COLOR_SPACE_TYPE outSpace) {
    const auto *in=video_format_row(inFormat);
    const auto *out=video_format_row(outFormat);
    if (!in || !out || !in->input || !out->output) return false;
    const auto a=video_color_space(inSpace), b=video_color_space(outSpace);
    return a.admitted && b.admitted && a.ycbcr==in->ycbcr && b.ycbcr==out->ycbcr;
}
// GetVideoProcessorCaps. DeviceCaps: the transform converts the YCbCr matrix (601/709), the nominal range of YCbCr
// and the RGB range (engine fix t38/video-blit). FeatureCaps: none. The engine has a background colour and an alpha
// fill mode in its state, but its blit writes neither, so ALPHA_FILL is not claimed. One input stream, one stream
// state: the engine draws the enabled streams one after the other with no blending between them.
constexpr D3D11_1DDI_VIDEO_PROCESSOR_CAPS video_processor_caps() {
    D3D11_1DDI_VIDEO_PROCESSOR_CAPS c{};
    c.DeviceCaps=D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_RGB_RANGE_CONVERSION |
                 D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_YCbCr_MATRIX_CONVERSION |
                 D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_NOMINAL_RANGE;
    c.FeatureCaps=0;
    c.FilterCaps=0;
    c.InputFormatCaps=0;
    c.AutoStreamCaps=0;
    c.StereoCaps=0;
    c.RateConversionCapsCount=1;
    c.MaxInputStreams=1;
    c.MaxStreamStates=1;
    return c;
}
// The one rate conversion entry: progressive frames, no past or future frames, no deinterlacing, no inverse
// telecine, no custom rates. ConversionCaps 0 is the processor that only copies, scales and converts colour.
constexpr D3D11_1DDI_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS video_rate_conversion_caps() {
    return D3D11_1DDI_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS{};
}
// The content descriptions the enumerator admits: sizes the engine can make a 2D image of. CreateVideoProcessorEnum
// may answer only S_OK or E_OUTOFMEMORY, so an interlaced description is admitted too: its one rate conversion
// entry has no deinterlacing (ConversionCaps 0), which tells the application that the processor shows a frame as it
// is (a known deviation: an interlaced frame is not deinterlaced).
constexpr bool video_content_admitted(const D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC &d) {
    constexpr UINT kMaxSize=16384; // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
    return d.InputWidth && d.InputHeight && d.OutputWidth && d.OutputHeight && d.InputWidth<=kMaxSize &&
           d.InputHeight<=kMaxSize && d.OutputWidth<=kMaxSize && d.OutputHeight<=kMaxSize;
}
}
