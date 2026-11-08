// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: Linux amdgpu DC, MIT, revision 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
// Compose the existing imported preparation adapters, without hardware access.
#include "dcn_overlay.h"
#include "dcn_plane.h"
#include "../contract/bc250_nv12_layout.h"

static int OverlayAxis(const BC250_DCN_CAPS* Caps, int Nv12, unsigned Limit,
    BC250_SCALER_AXIS_INPUT* Axis, unsigned Surface, unsigned Active,
    int SourceBegin, int SourceEnd, int DestinationBegin, int DestinationEnd, int ClipBegin, int ClipEnd)
{
    unsigned source, destination;
    unsigned up = Nv12 ? Caps->MaxUpscaleNv12 : Caps->MaxUpscaleArgb;
    unsigned down = Nv12 ? Caps->MaxDownscaleNv12 : Caps->MaxDownscaleArgb;
    if (!up || !down || !Active || Active > Limit || Surface < Caps->MinSurfaceDimension || Surface > Limit ||
        SourceBegin < 0 || SourceEnd <= SourceBegin || (unsigned)SourceEnd > Surface ||
        DestinationEnd <= DestinationBegin || ClipEnd <= ClipBegin) return 0;
    // Subtract in 64 bits: hostile signed endpoints must not overflow first.
    if ((long long)DestinationEnd - DestinationBegin > (long long)Limit) return 0;
    source = (unsigned)(SourceEnd - SourceBegin);
    destination = (unsigned)((long long)DestinationEnd - DestinationBegin);
    if ((unsigned long long)destination * 1000u > (unsigned long long)source * up ||
        (unsigned long long)destination * 1000u < (unsigned long long)source * down)
        return 0;
    *Axis = (BC250_SCALER_AXIS_INPUT){Surface, Active, SourceBegin, SourceEnd,
        DestinationBegin, DestinationEnd, ClipBegin, ClipEnd, 0, 0};
    return 1;
}

static unsigned OverlayPrepare(const BC250_OVERLAY_REQUEST* Request,
    void* Workspace, size_t WorkspaceBytes, BC250_OVERLAY_PLAN* Plan, int Resident)
{
    BC250_OVERLAY_REQUEST in = {0};
    BC250_DML201_PLANE_INPUT dml[2] = {0};
    BC250_MPC_REQUEST mpc = {0};
    unsigned result = BC250_OVERLAY_ARGUMENT, pipes = 0, layers = 0;
    const BC250_DCN_CAPS* caps;
    if (!Plan) return result;
    if (Request) in = *Request;
    *Plan = (BC250_OVERLAY_PLAN){0};
    if (!Request || !Workspace || !in.Caps) return result;
    caps = in.Caps;
    result = BC250_OVERLAY_TOPOLOGY;
    // This planner owns exactly two pipes: the primary and one slave plane.
    if (caps->Pipes < 2u || caps->MaxSlavePlanes < 1u) goto refused;
    if (in.Count != 2 || in.SourceId || in.PostCompositionChanged) goto refused;
    for (unsigned i = 0; i < 2; ++i) {
        const BC250_OVERLAY_INPUT* p = &in.Plane[i];
        BC250_SCALER_REQUEST scaler = {0};
        BC250_CNVC_REQUEST color = {p->Format, p->Color};
        unsigned pipe = p->Format == BC250_CNVC_NV12 ? 1u : 0u;
        result = BC250_OVERLAY_TOPOLOGY;
        if (p->LayerIndex > 1 || (layers & (1u << p->LayerIndex)) || (pipes & (1u << pipe))) goto refused;
        layers |= 1u << p->LayerIndex; pipes |= 1u << pipe;
        result = BC250_OVERLAY_FLAGS;
        if (p->UnsupportedFlags) goto refused;
        result = BC250_OVERLAY_FORMAT;
        if (pipe && (caps->Nv12 == BC250_DCN_NV12_NONE || !caps->MaxSlaveYuvPlanes)) goto refused;
        if (!pipe && p->Format != BC250_LINEAR_BGRA8 && p->Format != BC250_LINEAR_RGBA8 &&
            p->Format != BC250_LINEAR_BGRX8) goto refused;
        result = BC250_OVERLAY_LAYOUT;
        if (!Resident) {
            // No MC/Vram value participates in metadata admission.
            if (pipe) {
                BC250_NV12_STORAGE storage={p->Width,p->Height,p->PitchBytes,p->ChromaPitchBytes,
                    p->AllocationBytes,p->SurfaceOffset,p->ChromaOffset};
                if (!Bc250Nv12StorageValid(&storage)) goto refused;
            } else {
                unsigned long long span=(unsigned long long)p->PitchBytes*p->Height;
                if (!p->Width || !p->Height || p->Width>caps->MaxSurfaceWidth || p->Height>caps->MaxSurfaceHeight ||
                    !caps->PitchAlignBytes || p->PitchBytes%caps->PitchAlignBytes ||
                    p->PitchBytes<(unsigned long long)p->Width*4u || p->PitchBytes>caps->MaxPitchBytes || p->ChromaPitchBytes || p->ChromaOffset ||
                    (p->SurfaceOffset&255ull) || p->SurfaceOffset>p->AllocationBytes ||
                    span>p->AllocationBytes-p->SurfaceOffset) goto refused;
            }
        } else if (pipe) {
            BC250_NV12_LAYOUT_REQUEST layout = {p->Width,p->Height,p->PitchBytes,p->ChromaPitchBytes,
                p->AllocationMcAddress,p->AllocationBytes,p->SurfaceOffset,p->ChromaOffset,
                in.McBase,in.VramPhysical,in.VramLength};
            if (!Bc250Nv12LayoutPrepare(&layout,&Plan->Nv12)) goto refused;
        } else {
            BC250_SURFACE_LAYOUT_REQUEST layout = {p->Format,p->Width,p->Height,p->PitchBytes,0,0,0,
                p->AllocationMcAddress,p->AllocationBytes,p->SurfaceOffset,in.McBase,in.VramPhysical,in.VramLength};
            if (p->ChromaPitchBytes || p->ChromaOffset || !Bc250SurfaceLayoutPrepare(&layout,&Plan->Rgb)) goto refused;
        }
        // The shared layout contracts keep the register field limits; the row may only narrow them.
        if (p->Width > caps->MaxSurfaceWidth || p->Height > caps->MaxSurfaceHeight) goto refused;
        result = BC250_OVERLAY_COLOR;
        if (!Bc250CnvcPrepare(&color,&Plan->Color[pipe])) goto refused;
        result = BC250_OVERLAY_GEOMETRY;
        if (!OverlayAxis(caps,(int)pipe,caps->MaxSurfaceWidth,&scaler.Geometry.Horizontal,p->Width,in.Timing.HActive,
                p->Source.Left,p->Source.Right,p->Destination.Left,p->Destination.Right,p->Clip.Left,p->Clip.Right) ||
            !OverlayAxis(caps,(int)pipe,caps->MaxSurfaceHeight,&scaler.Geometry.Vertical,p->Height,in.Timing.VActive,
                p->Source.Top,p->Source.Bottom,p->Destination.Top,p->Destination.Bottom,p->Clip.Top,p->Clip.Bottom)) goto refused;
        if ((long long)p->Source.Right - p->Source.Left > (long long)caps->MaxDownscaleSourceWidth &&
            (long long)p->Destination.Right - p->Destination.Left < (long long)p->Source.Right - p->Source.Left) goto refused;
        scaler.Geometry.ChromaDivisor = pipe ? 2u : 1u;
        scaler.FilterKind = p->Filter;
        if (Bc250DcnGeometryPrepare(&scaler,pipe ? BC250_DCN_YCBCR420 : BC250_DCN_RGB,
            &Plan->Geometry[pipe]) != BC250_SCALER_GEOMETRY_OK) goto refused;
        Plan->LayerByPipe[pipe] = p->LayerIndex;
        mpc.Plane[pipe].Enabled = 1;
        mpc.Plane[pipe].ZOrder = 1u - p->LayerIndex;
        dml[pipe] = (BC250_DML201_PLANE_INPUT){&Plan->Geometry[pipe].Scaler,p->Width,p->Height,
            pipe ? p->PitchBytes : p->PitchBytes / 4u,
            (unsigned)(p->Source.Right-p->Source.Left),(unsigned)(p->Source.Bottom-p->Source.Top),
            pipe ? 8u : 32u,pipe ? BC250_DML201_NV12 : BC250_DML201_RGB32,
            pipe ? p->ChromaPitchBytes / 2u : 0u};
    }
    result = BC250_OVERLAY_TOPOLOGY;
    if (!Bc250MpcPrepare(&mpc,&Plan->Mpc)) goto refused;
    result = BC250_OVERLAY_DML;
    if (Bc250Dml201PlanGroup(&in.Timing,dml,2,Workspace,WorkspaceBytes,&Plan->Dml) != BC250_DML201_OK) goto refused;
    Plan->Described = 1;
    Plan->Ready = Resident ? 1u : 0u;
    return BC250_OVERLAY_OK;
refused:
    *Plan = (BC250_OVERLAY_PLAN){0};
    return result;
}

unsigned Bc250OverlayPrepare(const BC250_OVERLAY_REQUEST* Request,
    void* Workspace, size_t WorkspaceBytes, BC250_OVERLAY_PLAN* Plan)
{
    return OverlayPrepare(Request,Workspace,WorkspaceBytes,Plan,1);
}

unsigned Bc250OverlayDescribe(const BC250_OVERLAY_DESCRIPTION* Description,
    const BC250_DCN_CAPS* Caps, const BC250_DML201_INPUT* Timing, void* Workspace, size_t WorkspaceBytes,
    BC250_OVERLAY_PLAN* Plan)
{
    BC250_OVERLAY_REQUEST request={0};
    if (!Description || !Timing || !Caps) {
        if (Plan) *Plan=(BC250_OVERLAY_PLAN){0};
        return BC250_OVERLAY_ARGUMENT;
    }
    request.Count=Description->Count; request.SourceId=Description->SourceId;
    request.PostCompositionChanged=Description->PostCompositionChanged;
    request.Timing=*Timing; request.Caps=Caps;
    request.Plane[0]=Description->Plane[0]; request.Plane[1]=Description->Plane[1];
    return OverlayPrepare(&request,Workspace,WorkspaceBytes,Plan,0);
}
