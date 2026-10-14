// BD-155: real timing/pipe admission, inherited-only connector admission and
// VidPN predicates. The three scaled tuples are synthetic host inputs based on
// the observed source/target classes; no fixture is a hardware pass.
static void InheritedStartCases(void)
{
    // Scaled tuples only: the inherited route is for a transform the firmware
    // applied. The equal-size 304.25 MHz tuple that 0.7.216.64 sent down this
    // route moved to inherited_bd169_cases.h, where the start keeps it on the
    // static path with its own source mode list (BD-169).
    static const struct { ULONG sw,sh,pitch,tw,th,ht,vt,pixel; } cases[]={
        {800,600,3328,1920,1080,2080,1119,174490000},
        {800,600,3328,2560,1440,2720,1492,304250000},
        {1024,768,4096,1920,1080,2080,1119,174490000}
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
            // Manual mode (low bits zero) with unity ratios and an enlarged
            // recout stays CUSTOM; inactive NUM_PIPE=1 does not mean autoscale.
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
        path.ContentTransformation.Scaling=autocal?D3DKMDT_VPPS_CUSTOM:D3DKMDT_VPPS_STRETCHED;
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
        d.Modeset.Modes.Count=0;CHECK(ConnectorStartInherited(&d)==STATUS_SUCCESS && d.Modeset.Modes.Count==1);
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
    // Native admission without EDID must still serve the saved firmware tuple.
    // Exercise the actual route selector and admission, including unavailable
    // programming prerequisites and the ordinary programmable positive control.
    for(i=0;i<9;i++) {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        BC250_INHERITED_PIPE saved;ULONG support;
        DXGKARG_COMMITVIDPN args={0};
        Mode(&d,1920,1080,2200,1125,148500000);
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS && !d.Modeset.InheritedFixed);
        CHECK(d.Modeset.InheritedSupport&BC250_SCALING_BIT(BC250_SCALING_IDENTITY));
        saved=d.Modeset.InheritedPipe;support=d.Modeset.InheritedSupport;
        d.Modeset.Requested=BC250_DISPLAY_MODES_SCALED;d.Modeset.Level=BC250_DISPLAY_MODES_SCALED;
        d.Modeset.EdidValid=TRUE;d.Modeset.DpcdValid=TRUE;d.DcnWriteEnabled=TRUE;
        d.Dxgk.DxgkCbSynchronizeExecution=&d;d.Dxgk.DxgkCbQueueDpc=&d;d.Dxgk.DxgkCbIndicateChildStatus=&d;
        CHECK(!ConnectorNeedsInherited(&d));
        // An earlier source list can contain additional modes. None may escape
        // the inherited-only route once connector programming is unavailable.
        d.Modeset.Modes.Count=2;
        d.Modeset.Modes.Modes[0].Width=1920;d.Modeset.Modes.Modes[0].Height=1080;
        d.Modeset.Modes.Modes[1].Width=1280;d.Modeset.Modes.Modes[1].Height=720;
        if(i==0) d.Modeset.EdidValid=FALSE;
        if(i==1) d.Modeset.DpcdValid=FALSE; // adapter remains unknown
        if(i==2) d.DcnWriteEnabled=FALSE;
        if(i==3) d.Dxgk.DxgkCbSynchronizeExecution=NULL;
        if(i==4) d.Dxgk.DxgkCbQueueDpc=NULL;
        if(i==5) d.Dxgk.DxgkCbIndicateChildStatus=NULL;
        if(i==6) d.Mmio=NULL;
        if(i==7) d.Modeset.Requested=BC250_DISPLAY_MODES_OFF;
        if(i==8) d.Modeset.InheritedFixed=TRUE;
        CHECK(ConnectorNeedsInherited(&d));
        CHECK(ConnectorStartInherited(&d)==STATUS_SUCCESS && d.Connector.StartAccepted==1);
        CHECK(d.Modeset.InheritedFixed && d.Modeset.Level==BC250_DISPLAY_MODES_OFF);
        CHECK(d.Modeset.Modes.Count==1 && d.Modeset.Modes.Modes[0].Width==1920 &&
            d.Modeset.Modes.Modes[0].Height==1080 && d.Modeset.Scaling==BC250_SCALING_IDENTITY);
        CHECK(ModesetScalingSupport(&d,1280,720)==0);
        CHECK(ModesetCommit(&d,1280,720,BC250_SCALING_IDENTITY)<0);
        memset(&commitSource,0,sizeof(commitSource));memset(&commitPath,0,sizeof(commitPath));
        commitSource.Type=D3DKMDT_RMT_GRAPHICS;
        commitSource.Format.Graphics.PrimSurfSize.cx=1920;commitSource.Format.Graphics.PrimSurfSize.cy=1080;
        commitSource.Format.Graphics.VisibleRegionSize=commitSource.Format.Graphics.PrimSurfSize;
        commitSource.Format.Graphics.Stride=d.Post.Pitch;commitSource.Format.Graphics.PixelFormat=21;
        commitPath.VidPnTargetId=BC250_CHILD_UID;
        commitPath.ContentTransformation.Rotation=D3DKMDT_VPPR_IDENTITY;
        commitPath.ContentTransformation.Scaling=D3DKMDT_VPPS_IDENTITY;commitTarget=signal;
        d.Dxgk.DxgkCbQueryVidPnInterface=CommitQueryInterface;args.hFunctionalVidPn=&d;
        CHECK(CommitVidPnCore(&d,&args)==STATUS_SUCCESS && d.ModeActive);
        CHECK(!memcmp(&saved,&d.Modeset.InheritedPipe,sizeof(saved)) && d.Modeset.InheritedSupport==support);
        CHECK(pipeWrites==0 && commitPrograms==0 && commitTransactions==0);
        d.Modeset.InheritedSupport=0;
        CHECK(ConnectorStartInherited(&d)==STATUS_NOT_SUPPORTED);
        d.Modeset.InheritedSupport=1ul<<BC250_SCALING_COUNT;
        CHECK(ConnectorStartInherited(&d)==STATUS_NOT_SUPPORTED);
    }
}
