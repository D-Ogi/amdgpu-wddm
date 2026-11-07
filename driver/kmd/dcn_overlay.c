// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: Linux amdgpu DC, MIT, revision 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
// Compose the existing imported preparation adapters, without hardware access.
#include "dcn_overlay.h"
#include "dcn_plane.h"

static int OverlayAxis(BC250_SCALER_AXIS_INPUT* Axis, unsigned Surface, unsigned Active,
    int SourceBegin, int SourceEnd, int DestinationBegin, int DestinationEnd, int ClipBegin, int ClipEnd)
{
    unsigned source, destination;
    if (!Active || Active > 16383u || Surface < BC250_PLANE_MIN_DIMENSION || Surface > 16383u ||
        SourceBegin < 0 || SourceEnd <= SourceBegin || (unsigned)SourceEnd > Surface ||
        DestinationEnd <= DestinationBegin || ClipEnd <= ClipBegin) return 0;
    // Subtract in 64 bits: hostile signed endpoints must not overflow first.
    if ((long long)DestinationEnd - DestinationBegin > 16383) return 0;
    source = (unsigned)(SourceEnd - SourceBegin);
    destination = (unsigned)((long long)DestinationEnd - DestinationBegin);
    if ((unsigned long long)destination * 1000u > (unsigned long long)source * BC250_PLANE_MAX_UPSCALE_MILLI ||
        (unsigned long long)destination * 1000u < (unsigned long long)source * BC250_PLANE_MAX_DOWNSCALE_MILLI)
        return 0;
    *Axis = (BC250_SCALER_AXIS_INPUT){Surface, Active, SourceBegin, SourceEnd,
        DestinationBegin, DestinationEnd, ClipBegin, ClipEnd, 0, 0};
    return 1;
}

unsigned Bc250OverlayPrepare(const BC250_OVERLAY_REQUEST* Request,
    void* Workspace, size_t WorkspaceBytes, BC250_OVERLAY_PLAN* Plan)
{
    BC250_OVERLAY_REQUEST in = {0};
    BC250_DML201_PLANE_INPUT dml[2] = {0};
    BC250_MPC_REQUEST mpc = {0};
    unsigned result = BC250_OVERLAY_ARGUMENT, pipes = 0, layers = 0;
    if (!Plan) return result;
    if (Request) in = *Request;
    *Plan = (BC250_OVERLAY_PLAN){0};
    if (!Request || !Workspace) return result;
    result = BC250_OVERLAY_TOPOLOGY;
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
        if (!pipe && p->Format != BC250_LINEAR_BGRA8 && p->Format != BC250_LINEAR_RGBA8 &&
            p->Format != BC250_LINEAR_BGRX8) goto refused;
        result = BC250_OVERLAY_LAYOUT;
        if (pipe) {
            BC250_NV12_LAYOUT_REQUEST layout = {p->Width,p->Height,p->PitchBytes,p->ChromaPitchBytes,
                p->AllocationMcAddress,p->AllocationBytes,p->SurfaceOffset,p->ChromaOffset,
                in.McBase,in.VramPhysical,in.VramLength};
            if (!Bc250Nv12LayoutPrepare(&layout,&Plan->Nv12)) goto refused;
        } else {
            BC250_SURFACE_LAYOUT_REQUEST layout = {p->Format,p->Width,p->Height,p->PitchBytes,0,0,0,
                p->AllocationMcAddress,p->AllocationBytes,p->SurfaceOffset,in.McBase,in.VramPhysical,in.VramLength};
            if (p->ChromaPitchBytes || p->ChromaOffset || !Bc250SurfaceLayoutPrepare(&layout,&Plan->Rgb)) goto refused;
        }
        result = BC250_OVERLAY_COLOR;
        if (!Bc250CnvcPrepare(&color,&Plan->Color[pipe])) goto refused;
        result = BC250_OVERLAY_GEOMETRY;
        if (!OverlayAxis(&scaler.Geometry.Horizontal,p->Width,in.Timing.HActive,
                p->Source.Left,p->Source.Right,p->Destination.Left,p->Destination.Right,p->Clip.Left,p->Clip.Right) ||
            !OverlayAxis(&scaler.Geometry.Vertical,p->Height,in.Timing.VActive,
                p->Source.Top,p->Source.Bottom,p->Destination.Top,p->Destination.Bottom,p->Clip.Top,p->Clip.Bottom)) goto refused;
        if (p->Source.Right - p->Source.Left > (int)BC250_PLANE_MAX_DOWNSCALE_SOURCE_WIDTH &&
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
    Plan->Ready = 1;
    return BC250_OVERLAY_OK;
refused:
    *Plan = (BC250_OVERLAY_PLAN){0};
    return result;
}
