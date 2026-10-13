// BD-155: real timing/pipe admission, inherited-only connector admission and
// VidPN predicates. The three scaled tuples are synthetic host inputs based on
// the observed source/target classes; no fixture is a hardware pass.
static void InheritedStartCases(void)
{
    static const struct { ULONG sw,sh,pitch,tw,th,ht,vt,pixel; } cases[]={
        {800,600,3328,1920,1080,2080,1119,174490000},
        {800,600,3328,2560,1440,2720,1492,304250000},
        {1024,768,4096,1920,1080,2080,1119,174490000},
        {2560,1440,10240,2560,1440,2720,1492,304250000}
    };
    unsigned i,autocal;
    for(autocal=0;autocal<=1;autocal++)for(i=0;i<RTL_NUMBER_OF(cases);i++) {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        D3DKMDT_VIDPN_SOURCE_MODE mode={0};D3DKMDT_VIDPN_PRESENT_PATH path={0};
        DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH update={0};
        ULONG support;BC250_INHERITED_PIPE saved;
        Mode(&d,cases[i].tw,cases[i].th,cases[i].ht,cases[i].vt,cases[i].pixel);
        d.Post.Width=cases[i].sw;d.Post.Height=cases[i].sh;d.Post.Pitch=cases[i].pitch;
        PipeFixture(d.Post.Width,d.Post.Height,cases[i].tw,cases[i].th);
        pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=d.Post.Pitch/4-1;
        if(autocal) {
            // The firmware automatic scaler advertises CUSTOM, not a ratio
            // re-derived from the dimensions. These are not a programming plan.
            pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=0x01000000;
            pipeRegisters[BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO/4]=0x01000000;
            pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_AUTOCAL/4]=0x100;
        }
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS && d.Modeset.InheritedFixed);
        CHECK(signal.PixelRate==cases[i].pixel && signal.ActiveSize.cx==cases[i].tw);
        saved=d.Modeset.InheritedPipe;support=d.Modeset.InheritedSupport;
        CHECK(support!=0 && pipeWrites==0);
        Bc250ModeListBuild(d.Post.Width,d.Post.Height,NULL,BC250_DISPLAY_MODES_OFF,&d.Modeset.Modes);
        CHECK(ConnectorStartInherited(&d)==STATUS_SUCCESS && d.Connector.StartAccepted==1);
        CHECK(d.Modeset.Modes.Count==1 && d.Modeset.Modes.Modes[0].Width==d.Post.Width);
        mode.Type=D3DKMDT_RMT_GRAPHICS;mode.Format.Graphics.PrimSurfSize.cx=d.Post.Width;
        mode.Format.Graphics.PrimSurfSize.cy=d.Post.Height;
        mode.Format.Graphics.VisibleRegionSize=mode.Format.Graphics.PrimSurfSize;
        mode.Format.Graphics.Stride=d.Post.Pitch;mode.Format.Graphics.PixelFormat=21;
        path.VidPnTargetId=BC250_CHILD_UID;path.ContentTransformation.Rotation=D3DKMDT_VPPR_IDENTITY;
        path.ContentTransformation.Scaling=i==3?D3DKMDT_VPPS_IDENTITY:
            autocal?D3DKMDT_VPPS_CUSTOM:D3DKMDT_VPPS_STRETCHED;
        CHECK(DisplayInheritedModeMatches(&d,&mode,&path,&signal));
        {
            DXGKARG_COMMITVIDPN args={0};
            d.Dxgk.DxgkCbQueryVidPnInterface=CommitQueryInterface;
            args.hFunctionalVidPn=&d;commitSource=mode;commitPath=path;commitTarget=signal;
            CHECK(CommitVidPnCore(&d,&args)==STATUS_SUCCESS && d.ModeActive && d.CommitSeen);
            CHECK(d.CommittedSourceWidth==d.Post.Width && d.CommittedSourceHeight==d.Post.Height);
            commitSource.Format.Graphics.Stride+=256;
            CHECK(CommitVidPnCore(&d,&args)<0 && !d.ModeActive);
            commitSource=mode;commitTarget.PixelRate++;
            CHECK(CommitVidPnCore(&d,&args)<0 && !d.ModeActive);
            commitTarget=signal;commitPath.VidPnTargetId=0;
            CHECK(CommitVidPnCore(&d,&args)<0 && !d.ModeActive);
            commitPath=path;
            CHECK(CommitVidPnCore(&d,&args)==STATUS_SUCCESS && d.ModeActive);
        }
        update.VidPnPresentPathInfo=path;
        CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==STATUS_SUCCESS);
        CHECK(ModesetCommit(&d,d.Post.Width,d.Post.Height,ScalingFromPath(path.ContentTransformation.Scaling))==STATUS_SUCCESS);
        CHECK(ModesetCommit(&d,d.Post.Width+64,d.Post.Height,BC250_SCALING_IDENTITY)<0);
        path.VidPnTargetId=0;CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&signal));
        path.VidPnTargetId=BC250_CHILD_UID;mode.Format.Graphics.Stride+=256;
        CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&signal));
        d.Modeset.Modes.Count=0;CHECK(ConnectorStartInherited(&d)==STATUS_NOT_SUPPORTED);
        d.Modeset.Modes.Count=1;d.InheritedSignalValid=FALSE;
        CHECK(ConnectorStartInherited(&d)==STATUS_NOT_SUPPORTED);d.InheritedSignalValid=TRUE;
        d.Modeset.InheritedPipeStatus=STATUS_DEVICE_BUSY;
        CHECK(ConnectorStartInherited(&d)==STATUS_NOT_SUPPORTED);d.Modeset.InheritedPipeStatus=STATUS_SUCCESS;
        CHECK(!memcmp(&saved,&d.Modeset.InheritedPipe,sizeof(saved)) && d.Modeset.InheritedSupport==support);
        CHECK(pipeWrites==0 && commitPrograms==0 && commitTransactions==0);
    }
    {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        Mode(&d,1920,1080,2200,1125,148500000);
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS && !d.Modeset.InheritedFixed);
        CHECK(d.Post.Pitch==7680 && pipeWrites==0);
    }
}
