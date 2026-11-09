// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// Included by wddm.c after the flip state machine block: the WDDM side of the two-plane Check3 preflight (BD-223
// Part Y). It normalises two WDDM planes into an integer-only BC250_OVERLAY_DESCRIPTION for the connector.
// The two-plane Check3 path consumes owned copies, never an allocation pointer
// held across connector/FPU work. A successful preflight still does not admit
// scanout: clock/pipe transaction ownership and Y0 hardware proof are pending.
static NTSTATUS WddmMpo3CheckGroup(BC250_DEVICE* Device,BC250_WDDM* Wddm,
    const DXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3* Check,
    const DXGK_MULTIPLANE_OVERLAY_POST_COMPOSITION* Post,BC250_LINK_OVERLAY_CHECK* Out)
{
    HANDLE handles[2];BC250_MPO_STORAGE storage[2];BC250_OVERLAY_DESCRIPTION description={0};UINT i;
    RtlZeroMemory(Out,sizeof(*Out));
    if (Check->PlaneCount!=2 || !Check->ppPlanes || !Check->ppPlanes[0] || !Check->ppPlanes[1])
        return STATUS_INVALID_PARAMETER;
    handles[0]=Check->ppPlanes[0]->hAllocation;handles[1]=Check->ppPlanes[1]->hAllocation;
    if (!WddmSnapshotMpoStorage(Device,Wddm,handles,storage)) return STATUS_INVALID_PARAMETER;
    description.Count=2;
    for (i=0;i<2;i++) {
        const DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE2* p=Check->ppPlanes[i];
        const DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES3* a=&p->PlaneAttributes;
        BC250_OVERLAY_INPUT* out=&description.Plane[i];BC250_MPO3_PLANE facts;
        const BC250_WDDM_ALLOCATION_PRIVATE* s=&storage[i].Surface;
        if (p->VidPnSourceId) return STATUS_INVALID_PARAMETER;
        WddmMpo3PlaneOf(Device,1,0,0,a,Post,&facts);
        description.PostCompositionChanged |= facts.PostPresent && (facts.PostFlags ||
            facts.PostRotation!=BC250_MPO3_ROTATION_IDENTITY ||
            !Bc250Mpo3WholeMode(&facts.PostSrc,facts.ModeWidth,facts.ModeHeight) ||
            !Bc250Mpo3WholeMode(&facts.PostDst,facts.ModeWidth,facts.ModeHeight));
        out->LayerIndex=p->LayerIndex;
        out->UnsupportedFlags=a->Flags.Value || a->Blend.Value || a->Rotation!=D3DDDI_ROTATION_IDENTITY ||
            (a->StretchQuality!=DXGK_MULTIPLANE_OVERLAY_STRETCH_QUALITY_BILINEAR &&
             a->StretchQuality!=DXGK_MULTIPLANE_OVERLAY_STRETCH_QUALITY_HIGH);
        out->Filter=BC250_OVERLAY_FILTER_AMD;
        out->Width=s->Width;out->Height=s->Height;out->PitchBytes=s->Pitch;out->AllocationBytes=s->Size;
        if (s->Format==BC250_NV12_D3DDDI_FORMAT && !storage[i].UmdAlloc &&
            storage[i].Nv12.Surface.Version==BC250_WDDM_NV12_VERSION) {
            out->Format=BC250_CNVC_NV12;out->ChromaPitchBytes=storage[i].Nv12.ChromaPitch;
            out->SurfaceOffset=storage[i].Nv12.SurfaceOffset;out->ChromaOffset=storage[i].Nv12.ChromaOffset;
            // D3DDDI_COLOR_SPACE_TYPE values in WDK 26100: left-sited 601/709
            // studio/full. CSC preparation is not proof of chroma siting/latch.
            switch ((unsigned)a->ColorSpaceType) {
            case 6:out->Color=BC250_INPUT_601_LIMITED;break;
            case 7:out->Color=BC250_INPUT_601_FULL;break;
            case 8:out->Color=BC250_INPUT_709_LIMITED;break;
            case 9:out->Color=BC250_INPUT_709_FULL;break;
            default:return STATUS_NOT_SUPPORTED;
            }
        } else {
            if (s->Format==D3DDDIFMT_A8R8G8B8) out->Format=BC250_LINEAR_BGRA8;
            else if (s->Format==D3DDDIFMT_X8R8G8B8) out->Format=BC250_LINEAR_BGRX8;
            else if (s->Format==D3DDDIFMT_A8B8G8R8) out->Format=BC250_LINEAR_RGBA8;
            else return STATUS_NOT_SUPPORTED;
            if (a->ColorSpaceType!=D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) return STATUS_NOT_SUPPORTED;
            out->Color=BC250_INPUT_RGB_FULL;
        }
        out->Source=(BC250_OVERLAY_RECT){a->SrcRect.left,a->SrcRect.top,a->SrcRect.right,a->SrcRect.bottom};
        out->Destination=(BC250_OVERLAY_RECT){a->DstRect.left,a->DstRect.top,a->DstRect.right,a->DstRect.bottom};
        out->Clip=(BC250_OVERLAY_RECT){a->ClipRect.left,a->ClipRect.top,a->ClipRect.right,a->ClipRect.bottom};
    }
    return ConnectorLinkOverlayCheck(Device,&description,Out);
}
