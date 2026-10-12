static void InheritedVidpnCases(void)
{
    BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO s;
    DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH update={0};
    D3DKMDT_VIDPN_SOURCE_MODE mode={0};D3DKMDT_VIDPN_PRESENT_PATH path={0},other;
    Mode(&d,1920,1080,2200,1125,148500000);
    d.Post.Width=1024;d.Post.Height=768;d.Post.Pitch=4096;PipeFixture(1024,768,1920,1080);
    CHECK(TestFillSignalInfo(&d,&s)==0 && d.InheritedSignalValid && d.Modeset.InheritedFixed);
    mode.Type=D3DKMDT_RMT_GRAPHICS;mode.Format.Graphics.PrimSurfSize.cx=1024;mode.Format.Graphics.PrimSurfSize.cy=768;
    mode.Format.Graphics.VisibleRegionSize=mode.Format.Graphics.PrimSurfSize;
    mode.Format.Graphics.Stride=4096;mode.Format.Graphics.PixelFormat=21;
    path.ContentTransformation.Scaling=D3DKMDT_VPPS_STRETCHED;path.ContentTransformation.Rotation=D3DKMDT_VPPR_IDENTITY;
    CHECK(DisplayInheritedModeMatches(&d,&mode,&path,&s));
    update.VidPnPresentPathInfo=path;
    CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==0);
    update.VidPnPresentPathInfo.VisibleFromActiveBROffset.cx=1;
    CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    update.VidPnPresentPathInfo=path;update.VidPnPresentPathInfo.VidPnSourceId=1;
    CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    update.VidPnPresentPathInfo=path;update.VidPnPresentPathInfo.VidPnTargetId=1;
    CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    update.VidPnPresentPathInfo=path;update.VidPnPresentPathInfo.ContentTransformation.Scaling=D3DKMDT_VPPS_IDENTITY;
    CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    update.VidPnPresentPathInfo.ContentTransformation.Scaling=D3DKMDT_VPPS_UNPINNED;
    CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==0);
    update.VidPnPresentPathInfo.VisibleFromActiveTLOffset.cy=1;
    CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    CHECK(commitPrograms==0 && commitTransactions==0 && pipeWrites==0);
    other=path;other.ContentTransformation.Scaling=D3DKMDT_VPPS_IDENTITY;
    CHECK(!DisplayInheritedModeMatches(&d,&mode,&other,&s));
    other=path;other.ContentTransformation.Scaling=253;
    CHECK(!DisplayInheritedModeMatches(&d,&mode,&other,&s));
    other=path;other.ContentTransformation.Rotation=2;CHECK(!DisplayInheritedModeMatches(&d,&mode,&other,&s));
    other=path;other.VidPnTargetId=1;CHECK(!DisplayInheritedModeMatches(&d,&mode,&other,&s));
    other=path;other.VidPnSourceId=1;CHECK(!DisplayInheritedModeMatches(&d,&mode,&other,&s));
    other=path;other.VisibleFromActiveTLOffset.cx=1;CHECK(!DisplayInheritedModeMatches(&d,&mode,&other,&s));
    other=path;other.VisibleFromActiveBROffset.cy=1;CHECK(!DisplayInheritedModeMatches(&d,&mode,&other,&s));
    mode.Format.Graphics.VisibleRegionSize.cx--;CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&s));
    mode.Format.Graphics.VisibleRegionSize.cx++;mode.Format.Graphics.Stride+=256;
    CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&s));mode.Format.Graphics.Stride-=256;
    mode.Format.Graphics.PixelFormat=0;CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&s));mode.Format.Graphics.PixelFormat=21;
    s.ActiveSize.cx--;CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&s));s=d.InheritedSignal;
    s.PixelRate++;CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&s));s=d.InheritedSignal;
    s.VSyncFreq.Numerator++;CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&s));s=d.InheritedSignal;
    s.HSyncFreq.Denominator=0;CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&s));s=d.InheritedSignal;
    s.VSyncFreq.Numerator/=100;s.VSyncFreq.Denominator/=100; // equivalent rational is allowed
    CHECK(DisplayInheritedModeMatches(&d,&mode,&path,&s));
    CHECK(ModesetValidateInherited(&d)==0 && pipeWrites==0);
    pipeRegisters[BC250_REG_DMU_HUBP0_DCHUBP_CNTL/4]=HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
    CHECK(ModesetValidateInherited(&d)==0 && pipeWrites==0);
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_START/4]=1;
    CHECK(ModesetValidateInherited(&d)==STATUS_DEVICE_BUSY && pipeWrites==0);
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_START/4]=0;
    values[TimingPhase]++;
    CHECK(ModesetValidateInherited(&d)==STATUS_DEVICE_BUSY && pipeWrites==0);
    CHECK(d.InheritedSignal.PixelRate==148500000); // never mutate the published signal
}
