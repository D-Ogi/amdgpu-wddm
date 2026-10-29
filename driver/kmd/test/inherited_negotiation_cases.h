static void NegotiationResetShape(BC250_DEVICE* d,ULONG width,ULONG height,ULONG pitch,
    ULONG targetWidth,ULONG targetHeight,BOOLEAN custom)
{
    D3DKMDT_VIDEO_SIGNAL_INFO signal;
    memset(d,0,sizeof(*d));
    if(targetWidth==2560)Mode(d,2560,1440,2720,1481,241500000);
    else Mode(d,1920,1080,2080,1119,174490000);
    d->Post.Width=width;d->Post.Height=height;d->Post.Pitch=pitch;
    PipeFixture(width,height,targetWidth,targetHeight);
    // #38 did not record AUTOCAL. Here it is an explicit synthetic extension.
    if(targetWidth==2560)Field38Fixture(1,0);
    pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=pitch/4-1;
    // AUTOCAL_MODE=1, as in both .64 lab arms; ratio words are inactive.
    pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_AUTOCAL/4]=1;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=0x01000000;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO/4]=0x01000000;
    // A left-aligned recout two pixels short is none of the standard shapes.
    if(custom)pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=(targetWidth-2)|(targetHeight<<16);
    CHECK(TestFillSignalInfo(d,&signal)==0);
    CHECK(d->Modeset.InheritedSupport==BC250_SCALING_BIT(custom?BC250_SCALING_CUSTOM:BC250_SCALING_STRETCHED));
    CHECK(ConnectorStartInherited(d)==0);
    d->ComposedSourceModes=TRUE;d->Dxgk.DxgkCbQueryVidPnInterface=CommitQueryInterface;
    memset(&commitPath,0,sizeof(commitPath));memset(&commitSource,0,sizeof(commitSource));
    memset(&commitTarget,0,sizeof(commitTarget));
    commitPath.VidPnTargetId=BC250_CHILD_UID;
    commitPath.ContentTransformation.Scaling=D3DKMDT_VPPS_UNPINNED;
    commitPath.ContentTransformation.Rotation=D3DKMDT_VPPR_UNPINNED;
    sourceIsPinned=targetIsPinned=FALSE;commitPaths=1;
    sourceCount=targetCount=sourceAssigned=targetAssigned=updateCalls=failBoundary=0;
    CHECK(!pathRefs && !sourceRefs && !targetRefs && !sourceInfoRefs && !targetInfoRefs);
}
static void NegotiationReset(BC250_DEVICE* d,ULONG width,ULONG height,ULONG pitch)
{
    NegotiationResetShape(d,width,height,pitch,1920,1080,TRUE);
}
static void NegotiationCases(void)
{
    BC250_DEVICE d;unsigned size,initial,pinOrder,pivot,n,custom;
    const unsigned unpinned[]={D3DKMDT_VPPS_UNPINNED,D3DKMDT_VPPS_UNINITIALIZED,D3DKMDT_VPPS_NOTSPECIFIED};
    for(custom=0;custom<=1;custom++)for(size=0;size<3;size++)
    for(initial=0;initial<RTL_NUMBER_OF(unpinned);initial++)for(pinOrder=0;pinOrder<2;pinOrder++) {
        DXGKARG_ISSUPPORTEDVIDPN supported={&d,FALSE};
        DXGKARG_ENUMVIDPNCOFUNCMODALITY cofunc={&d,D3DKMDT_EPT_NOPIVOT,{0,BC250_CHILD_UID}};
        DXGKARG_COMMITVIDPN commit={0};unsigned srcBefore,targetBefore;
        BC250_INHERITED_PIPE saved;
        NegotiationResetShape(&d,size==1?1024:800,size==1?768:600,size==1?4096:3328,
            size==2?2560:1920,size==2?1440:1080,(BOOLEAN)custom);saved=d.Modeset.InheritedPipe;
        commitPath.ContentTransformation.Scaling=unpinned[initial];
        commitPath.ContentTransformation.Rotation=unpinned[initial];
        CHECK(Bc250IsSupportedVidPn(&d,&supported)==0 && supported.IsVidPnSupported);
        CHECK(Bc250EnumVidPnCofuncModality(&d,&cofunc)==0);
        CHECK(sourceCount>0 && sourceCount<=BC250_SOURCE_MODE_MAX && targetCount==1 && sourceAssigned==1 && targetAssigned==1);
        CHECK(commitPath.ContentTransformation.Scaling==(custom?D3DKMDT_VPPS_CUSTOM:unpinned[initial]));
        CHECK((ULONG)commitPath.ContentTransformation.ScalingSupport.Custom==custom &&
            (ULONG)commitPath.ContentTransformation.ScalingSupport.Stretched==(ULONG)!custom &&
            !commitPath.ContentTransformation.ScalingSupport.Identity &&
            !commitPath.ContentTransformation.ScalingSupport.Centered &&
            !commitPath.ContentTransformation.ScalingSupport.AspectRatioCenteredMax);
        CHECK(commitPath.ContentTransformation.RotationSupport.Identity && commitPath.ContentTransformation.RotationSupport.Offset0);
        // BD-247: the three rotations, and no offset but 0 (E16 run 007).
        CHECK(commitPath.ContentTransformation.RotationSupport.Rotate90 && commitPath.ContentTransformation.RotationSupport.Rotate180 &&
            commitPath.ContentTransformation.RotationSupport.Rotate270 && !commitPath.ContentTransformation.RotationSupport.Offset90 &&
            !commitPath.ContentTransformation.RotationSupport.Offset180 && !commitPath.ContentTransformation.RotationSupport.Offset270);
        CHECK(offeredTarget.VideoSignalInfo.PixelRate==(size==2?241500000ull:174490000ull) &&
            offeredTarget.VideoSignalInfo.ActiveSize.cx==(size==2?2560ul:1920ul));
        CHECK(offeredTarget.WireFormatAndPreference.Rgb==D3DKMDT_BITS_PER_COMPONENT_08);
        for(n=0;n<sourceCount;n++) {
            CHECK(offeredSources[n].Format.Graphics.PrimSurfSize.cx==d.Post.Width &&
                offeredSources[n].Format.Graphics.PrimSurfSize.cy==d.Post.Height);
            CHECK(offeredSources[n].Format.Graphics.Stride==Bc250SourceModeStride(offeredSources[n].Format.Graphics.PixelFormat,d.Post.Pitch));
        }
        // OS choices are explicit test inputs, not an emulation of DMM policy.
        // Pin either source or target first and negotiate with the other unpinned.
        commitSource=offeredSources[0];commitTarget=offeredTarget.VideoSignalInfo;
        sourceIsPinned=pinOrder?FALSE:TRUE;targetIsPinned=pinOrder?TRUE:FALSE;
        srcBefore=sourceAssigned;targetBefore=targetAssigned;
        CHECK(Bc250IsSupportedVidPn(&d,&supported)==0 && supported.IsVidPnSupported);
        CHECK(Bc250EnumVidPnCofuncModality(&d,&cofunc)==0);
        CHECK(sourceAssigned==srcBefore+(pinOrder?1:0) && targetAssigned==targetBefore+(pinOrder?0:1));
        // DMM owns the standard scaling pin. A support update must not pretend
        // to pin it; our lab arms check that Windows actually selects it.
        commitPath.ContentTransformation.Scaling=custom?D3DKMDT_VPPS_CUSTOM:D3DKMDT_VPPS_STRETCHED;
        sourceIsPinned=targetIsPinned=TRUE;commitPath.ContentTransformation.Rotation=D3DKMDT_VPPR_IDENTITY;
        CHECK(Bc250IsSupportedVidPn(&d,&supported)==0 && supported.IsVidPnSupported);
        commit.hFunctionalVidPn=&d;
        CHECK(CommitVidPnCore(&d,&commit)==0 && d.ModeActive && d.CommitSeen);
        CHECK(d.CommittedSourceWidth==d.Post.Width && d.CommittedSourceHeight==d.Post.Height);
        CHECK(d.Modeset.Scaling==(ULONG)(custom?BC250_SCALING_CUSTOM:BC250_SCALING_STRETCHED));
        CHECK(!memcmp(&saved,&d.Modeset.InheritedPipe,sizeof(saved)) && !pipeWrites && !commitPrograms && !commitTransactions);
        CHECK(d.Rotation==D3DKMDT_VPPR_IDENTITY);
        // BD-247: the same commit with portrait pinned keeps the pipe and records the rotation.
        commitPath.ContentTransformation.Rotation=2;
        CHECK(Bc250IsSupportedVidPn(&d,&supported)==0 && supported.IsVidPnSupported);
        CHECK(CommitVidPnCore(&d,&commit)==0 && d.ModeActive && d.Rotation==2);
        CHECK(!memcmp(&saved,&d.Modeset.InheritedPipe,sizeof(saved)) && !pipeWrites && !commitPrograms && !commitTransactions);
    }
    for(pivot=D3DKMDT_EPT_VIDPNSOURCE;pivot<=D3DKMDT_EPT_ROTATION;pivot++) {
        DXGKARG_ENUMVIDPNCOFUNCMODALITY cofunc={&d,pivot,{0,BC250_CHILD_UID}};
        NegotiationReset(&d,800,600,3328);
        CHECK(Bc250EnumVidPnCofuncModality(&d,&cofunc)==0);
        CHECK(sourceAssigned==(pivot==D3DKMDT_EPT_VIDPNSOURCE?0u:1u));
        CHECK(targetAssigned==(pivot==D3DKMDT_EPT_VIDPNTARGET?0u:1u));
        if(pivot==D3DKMDT_EPT_SCALING) CHECK(commitPath.ContentTransformation.Scaling==D3DKMDT_VPPS_UNPINNED &&
            !commitPath.ContentTransformation.ScalingSupport.Custom);
        if(pivot==D3DKMDT_EPT_ROTATION) CHECK(!commitPath.ContentTransformation.RotationSupport.Identity &&
            !commitPath.ContentTransformation.RotationSupport.Offset0);
        cofunc.EnumPivotType=D3DKMDT_EPT_NOPIVOT;
        CHECK(Bc250EnumVidPnCofuncModality(&d,&cofunc)==0 && sourceCount && targetCount);
    }
    for(n=0;n<12;n++) {
        DXGKARG_ISSUPPORTEDVIDPN supported={&d,TRUE};
        DXGKARG_ENUMVIDPNCOFUNCMODALITY cofunc={&d,D3DKMDT_EPT_NOPIVOT,{0,BC250_CHILD_UID}};
        NegotiationReset(&d,1024,768,4096);
        CHECK(Bc250EnumVidPnCofuncModality(&d,&cofunc)==0);
        commitSource=offeredSources[0];commitTarget=offeredTarget.VideoSignalInfo;sourceIsPinned=targetIsPinned=TRUE;
        if(n==0)commitSource.Format.Graphics.PrimSurfSize.cx++;
        if(n==1)commitSource.Format.Graphics.Stride+=256;
        if(n==2)commitSource.Format.Graphics.VisibleRegionSize.cy--;
        if(n==3)commitSource.Format.Graphics.PixelFormat=0;
        if(n==4)commitSource.Type=0;
        if(n==5)commitTarget.PixelRate++;
        if(n==6)commitPath.VidPnTargetId=0;
        if(n==7)commitPath.VidPnSourceId=1;
        if(n==8)commitPath.ContentTransformation.Scaling=D3DKMDT_VPPS_IDENTITY;
        if(n==9)commitPath.ContentTransformation.Rotation=6;    /* BD-247: an offset; 3 (180) is admitted */
        if(n==10)commitPath.VisibleFromActiveBROffset.cx=1;
        if(n==11)commitPaths=2;
        CHECK(Bc250IsSupportedVidPn(&d,&supported)==0 && !supported.IsVidPnSupported);
        CHECK(Bc250EnumVidPnCofuncModality(&d,&cofunc)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
        CHECK(!pathRefs && !sourceRefs && !targetRefs && !sourceInfoRefs && !targetInfoRefs);
    }
    for(n=1;n<=5;n++) {
        DXGKARG_ENUMVIDPNCOFUNCMODALITY cofunc={&d,D3DKMDT_EPT_NOPIVOT,{0,BC250_CHILD_UID}};
        NegotiationReset(&d,800,600,3328);failBoundary=n;
        CHECK(Bc250EnumVidPnCofuncModality(&d,&cofunc)==STATUS_ACCESS_DENIED);
        CHECK(!pathRefs && !sourceRefs && !targetRefs && !sourceInfoRefs && !targetInfoRefs);
    }
    {
        DXGKARG_ISSUPPORTEDVIDPN supported={&d,FALSE};
        unsigned lines;
        NegotiationReset(&d,800,600,3328);
        for(n=0;n<16;n++)CHECK(Bc250IsSupportedVidPn(&d,&supported)==0 && supported.IsVidPnSupported);
        lines=guardLines;
        CHECK(Bc250IsSupportedVidPn(&d,&supported)==0 && guardLines==lines);
        CHECK(!pathRefs && !sourceRefs && !targetRefs && !sourceInfoRefs && !targetInfoRefs);
    }
    failBoundary=0;sourceIsPinned=targetIsPinned=TRUE;commitPaths=1;
    puts("inherited VidPN: three tuples, standard/custom pins, pivots, commit, refusal and ownership controls passed");
}
