// Included by display.c after the mode-set helpers. The inherited route offers
// one immutable tuple. Enumerating it never programs the display engine.
static NTSTATUS DisplayInheritedVidPn(BC250_DEVICE* Device, D3DKMDT_HVIDPN Handle,
    const DXGKARG_ENUMVIDPNCOFUNCMODALITY* Enum, BOOLEAN Commit, BOOLEAN* Supported)
{
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH* path = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH update = {0};
    D3DKMDT_VIDPN_SOURCE_MODE source = {0};
    D3DKMDT_VIDEO_SIGNAL_INFO target = {0};
    BOOLEAN sourcePinned = FALSE, targetPinned = FALSE, changed = FALSE;
    SIZE_T paths = 0;
    NTSTATUS status = STATUS_SUCCESS;
    const char* reason = "empty";
    LONG call = InterlockedIncrement(Commit ? &Device->Modeset.InheritedCommitCalls : Enum ? &Device->Modeset.InheritedCofuncCalls :
        &Device->Modeset.InheritedSupportCalls);
    const BOOLEAN log = call > 0 && call <= 16;
    const char op = Commit ? 'C' : Enum ? 'E' : 'S'; // commit / enum / supported
    ULONG scaling = D3DKMDT_VPPS_UNINITIALIZED;
    ULONG rotation = D3DKMDT_VPPR_UNINITIALIZED;
    const ULONG support = Device->Modeset.InheritedSupport;

    *Supported = FALSE;
    if (Commit) Device->ModeActive = FALSE;
    if (!Handle) { *Supported = TRUE; goto done; }
    reason = "query-interface";
    status = Device->Dxgk.DxgkCbQueryVidPnInterface(Handle, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) goto done;
    reason = "topology";
    status = vidpn->pfnGetTopology(Handle, &hTopology, &topology);
    if (!NT_SUCCESS(status)) goto done;
    reason = "path-count";
    status = topology->pfnGetNumPaths(hTopology, &paths);
    if (!NT_SUCCESS(status)) goto done;
    if (!paths) { *Supported = TRUE; reason = "empty"; goto done; }
    if (paths != 1) { status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; goto done; }
    reason = "acquire-path";
    status = topology->pfnAcquireFirstPathInfo(hTopology, &path);
    if (!NT_SUCCESS(status)) goto done;
    if (!path) { status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; goto done; }
    update = *path;
    scaling = path->ContentTransformation.Scaling;
    rotation = path->ContentTransformation.Rotation;
    reason = "path-ids";
    if (path->VidPnSourceId != 0 || path->VidPnTargetId != BC250_CHILD_UID) {
        status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; goto done;
    }
    reason = "source-query";
    status = SourceModeIsPinned(vidpn, Handle, path->VidPnSourceId, &sourcePinned, &source);
    if (!NT_SUCCESS(status)) goto done;
    reason = "target-query";
    status = TargetModeIsPinned(vidpn, Handle, path->VidPnTargetId, &targetPinned, &target);
    if (!NT_SUCCESS(status)) goto done;

    // Substitute the one admissible mode only for validation of an unpinned
    // set. The log preserves both pin flags. A pinned constraint is never replaced.
    if (!sourcePinned) {
        source.Type = D3DKMDT_RMT_GRAPHICS;
        source.Format.Graphics.PrimSurfSize.cx = Device->Post.Width;
        source.Format.Graphics.PrimSurfSize.cy = Device->Post.Height;
        source.Format.Graphics.VisibleRegionSize = source.Format.Graphics.PrimSurfSize;
        source.Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        source.Format.Graphics.Stride = Device->Post.Pitch;
    }
    if (!targetPinned) target = Device->InheritedSignal;
    reason = DisplayInheritedModeRefusal(Device, &source, path, &target);
    if (reason) { status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; goto done; }
    if (Commit) {
        reason = "commit-pins";
        if (!sourcePinned || !targetPinned) { status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED; goto done; }
        reason = "commit-mode";
        status = ModesetCommit(Device, source.Format.Graphics.PrimSurfSize.cx,
            source.Format.Graphics.PrimSurfSize.cy, ScalingFromPath(scaling));
        if (!NT_SUCCESS(status)) goto done;
        Device->CommittedSourceWidth = source.Format.Graphics.PrimSurfSize.cx;
        Device->CommittedSourceHeight = source.Format.Graphics.PrimSurfSize.cy;
        Device->CommittedSourceFormat = (ULONG)source.Format.Graphics.PixelFormat;
        Device->ModeActive = TRUE;
        *Supported = TRUE; reason = "ok"; goto done;
    }
    if (!Enum) { *Supported = TRUE; reason = "ok"; goto done; }

    reason = "offer-source";
    if (!sourcePinned && !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE &&
        Enum->EnumPivot.VidPnSourceId == path->VidPnSourceId))
        status = OfferSourceMode(Device, vidpn, Handle, path->VidPnSourceId, scaling);
    if (!NT_SUCCESS(status)) goto done;
    reason = "offer-target";
    if (!targetPinned && !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET &&
        Enum->EnumPivot.VidPnTargetId == path->VidPnTargetId))
        status = OfferTargetMode(Device, vidpn, Handle, path->VidPnTargetId);
    if (!NT_SUCCESS(status)) goto done;

    // Microsoft: Using Aspect Ratio and Custom Scaling Modes; enumeration
    // preserves pinned constraints and the selected pivot. CUSTOM alone is a
    // concrete inherited transformation, not an invitation to program identity.
    if (!ScalingPinned(scaling) && Enum->EnumPivotType != D3DKMDT_EPT_SCALING) {
        RtlZeroMemory(&update.ContentTransformation.ScalingSupport,
            sizeof(update.ContentTransformation.ScalingSupport));
        update.ContentTransformation.ScalingSupport.Identity = !!(support & BC250_SCALING_BIT(BC250_SCALING_IDENTITY));
        update.ContentTransformation.ScalingSupport.Centered = !!(support & BC250_SCALING_BIT(BC250_SCALING_CENTERED));
        update.ContentTransformation.ScalingSupport.Stretched = !!(support & BC250_SCALING_BIT(BC250_SCALING_STRETCHED));
        update.ContentTransformation.ScalingSupport.AspectRatioCenteredMax = !!(support & BC250_SCALING_BIT(BC250_SCALING_ASPECT));
        update.ContentTransformation.ScalingSupport.Custom = !!(support & BC250_SCALING_BIT(BC250_SCALING_CUSTOM));
        if (support == BC250_SCALING_BIT(BC250_SCALING_CUSTOM))
            update.ContentTransformation.Scaling = D3DKMDT_VPPS_CUSTOM;
        changed = TRUE;
    }
    if (Enum->EnumPivotType != D3DKMDT_EPT_ROTATION) {
        RtlZeroMemory(&update.ContentTransformation.RotationSupport,
            sizeof(update.ContentTransformation.RotationSupport));
        update.ContentTransformation.RotationSupport.Identity = 1;
        if (Device->FullWddm) update.ContentTransformation.RotationSupport.Offset0 = 1;
        changed = TRUE;
    }
    reason = "update-support";
    if (changed) status = topology->pfnUpdatePathSupportInfo(hTopology, &update);
    if (!NT_SUCCESS(status)) goto done;
    *Supported = TRUE;
    reason = "ok";
done:
    if (log) {
        GuardLog("ivpn %c%ld pivot %lu:%lu/%lu paths %lu",
            op, call, Enum ? (ULONG)Enum->EnumPivotType : MAXULONG,
            Enum ? Enum->EnumPivot.VidPnSourceId : MAXULONG,
            Enum ? Enum->EnumPivot.VidPnTargetId : MAXULONG, (ULONG)paths);
        GuardLog("ivpn %c%ld status %08lX supported %u why %s", op, call, (ULONG)status, *Supported, reason);
        if (path) {
            GuardLog("ivpn %c%ld path %lu/%lu scale %lu->%lu rot %lu update %u",
                op, call, path->VidPnSourceId, path->VidPnTargetId, scaling,
                (ULONG)update.ContentTransformation.Scaling, rotation, changed);
            GuardLog("ivpn %c%ld support I%u C%u S%u A%u X%u rot I%u O%u", op, call,
                update.ContentTransformation.ScalingSupport.Identity, update.ContentTransformation.ScalingSupport.Centered,
                update.ContentTransformation.ScalingSupport.Stretched, update.ContentTransformation.ScalingSupport.AspectRatioCenteredMax,
                update.ContentTransformation.ScalingSupport.Custom,
                update.ContentTransformation.RotationSupport.Identity, update.ContentTransformation.RotationSupport.Offset0);
            GuardLog("ivpn %c%ld source pinned %u type %u size %lux%lu",
                op, call, sourcePinned, source.Type, source.Format.Graphics.PrimSurfSize.cx,
                source.Format.Graphics.PrimSurfSize.cy);
            GuardLog("ivpn %c%ld source visible %lux%lu fmt %u pitch %lu", op, call, source.Format.Graphics.VisibleRegionSize.cx,
                source.Format.Graphics.VisibleRegionSize.cy, source.Format.Graphics.PixelFormat, source.Format.Graphics.Stride);
            GuardLog("ivpn %c%ld target pinned %u active %lux%lu total %lux%lu",
                op, call, targetPinned, target.ActiveSize.cx, target.ActiveSize.cy, target.TotalSize.cx, target.TotalSize.cy);
            GuardLog("ivpn %c%ld target pixel %Iu scan %u v %u/%u h %u/%u", op, call,
                target.PixelRate, target.ScanLineOrdering, target.VSyncFreq.Numerator, target.VSyncFreq.Denominator,
                target.HSyncFreq.Numerator, target.HSyncFreq.Denominator);
        }
    }
    if (path) topology->pfnReleasePathInfo(hTopology, path);
    return status;
}
