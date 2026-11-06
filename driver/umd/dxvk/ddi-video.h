// SPDX-License-Identifier: MIT
#pragma once
// The D3D11 video DDIs of this shell (tester.38, BD-223). The runtime asks for them through RetrieveSubObject
// (D3D10DDIARG_CREATEDEVICE::ppfnRetrieveSubObject), one sub ID for each table layout. Without that entry the
// runtime has no video table, ID3D11Device::QueryInterface(ID3D11VideoDevice) fails, and a browser reports no
// overlay support at all.
//
// The video processor is the engine's (DXVK ID3D11VideoDevice and ID3D11VideoContext). Each DDI object holds the
// engine COM object of the same kind; a view is made from the engine resource of the DDI resource
// (DdiResource::object). The caps, the formats and the colour spaces are this shell's own answers
// (ddi-video-caps.h), not the engine's: the engine's enumerator answers are stubs.
//
// There is no decoder: this GPU's VCN block is not driven by this driver. The decoder answers say so: no profile,
// no config, no content protection, and a create that the runtime should never send fails with a status its page
// allows.
#include "ddi-entry.h"
#include "ddi-video-caps.h"
#include <d3d11.h>
namespace bc250::umd {
struct DdiVideoProcessorEnum {
    ID3D11VideoProcessorEnumerator *object=nullptr;
};
struct DdiVideoProcessor {
    ID3D11VideoProcessor *object=nullptr;
};
struct DdiVideoProcessorInputView {
    ID3D11VideoProcessorInputView *object=nullptr;
};
struct DdiVideoProcessorOutputView {
    ID3D11VideoProcessorOutputView *object=nullptr;
};
// The sub IDs this shell answers and the size of the table each one asks for. The three layouts begin with the same
// members in the same order, so one table of the 2.1 layout fills each of them (static checks in ddi-video.cpp).
// The 2.4 tables (sub IDs 6 and 7) are not answered: this shell's highest interface is WDDM 2.2.
SIZE_T video_table_size(UINT32 subId);
// The one video table, in the WDDM 2.1 layout. Every member is set.
D3DWDDM2_1DDI_VIDEODEVICEFUNCS make_video_device_table();
HRESULT APIENTRY retrieve_subobject(D3D10DDI_HDEVICE,UINT32 subId,SIZE_T paramSize,void *params,
    SIZE_T outputSize,void *output);
// The conversions of the DDI arguments into the engine's API arguments. Pure, so the host test checks them.
D3D11_VIDEO_PROCESSOR_CONTENT_DESC convert_video_content(const D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC &);
D3D11_VIDEO_PROCESSOR_COLOR_SPACE convert_video_space(const D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE &);
D3D11_VIDEO_COLOR convert_video_color(const D3D11_1DDI_VIDEO_COLOR &);
// A video processor view of a 2D texture of `format`: false when the format is not one this processor reads (input)
// or writes (output), or the slice range is more than one slice (no stereo). The create entries answer
// DXGI_ERROR_UNSUPPORTED for false, the status their pages name for a view the driver cannot make.
bool plan_video_input_view(const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW &,DXGI_FORMAT format,
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC &);
bool plan_video_output_view(const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW &,DXGI_FORMAT format,
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC &);
// The statuses of the HRESULT entries that cannot succeed here, one per entry, each one named on the entry's
// reference page (ref/ddi-display/d3d10umddi.md). Where a page names no status, the create-type entries answer
// E_OUTOFMEMORY, the status every sibling create page names. ddi-video-test.cpp checks the table against the pages.
struct VideoRefusal { const char *entry; const char *page; HRESULT status; };
extern const VideoRefusal kVideoRefusals[];
extern const unsigned kVideoRefusalCount;
// The bounded diagnostic line of RetrieveSubObject (both flavours): which sub ID the runtime asked for, and whether
// this shell answered it. Lab arm A reads it to learn the layout the runtime takes at our interface.
unsigned video_subobject_notes_left();
}
