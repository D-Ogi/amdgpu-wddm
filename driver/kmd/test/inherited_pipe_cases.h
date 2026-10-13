// Retained 1920x1080 firmware tuple: 21 fields were read on the same start.
// AUTOCAL and DSCL_CONTROL were not captured then; PipeFixture supplies synthetic
// zeros for them. The separate UnitA49Fixture has the later complete 23-word read.
// Exercise the actual reader and classifier without changing the retained words.
static void InheritedSelfBottomCases(void)
{
    static const struct {ULONG offset,value;} measured[] = {
        {BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START,0x00000000},
        {BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION,0x04380780},
        {BC250_REG_DMU_DSCL0_RECOUT_START,0x00000000},
        {BC250_REG_DMU_DSCL0_RECOUT_SIZE,0x04380780},
        {BC250_REG_DMU_DSCL0_MPC_SIZE,0x04380780},
        {BC250_REG_DMU_DSCL0_SCL_MODE,0x00000001},
        {BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_CTRL,0x00000000},
        {BC250_REG_DMU_MPCC0_MPCC_TOP_SEL,0x00000000},
        {BC250_REG_DMU_ODM0_OPTC_DATA_SOURCE_SELECT,0x00000000},
        {BC250_REG_DMU_HUBP0_DCHUBP_CNTL,0x000F100A},
        {BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO,0x01000000},
        {BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO,0x01000000},
        {BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL,0x00000000},
        {BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG,0x00000008},
        {BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG,0x00000080},
        {BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,0x0000077F},
        {BC250_REG_DMU_OTG1_OTG_CONTROL,0x80000310},
        {BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL,0x00000000},
        {BC250_REG_DMU_MPCC0_MPCC_BOT_SEL,0x00000000},
        {BC250_REG_DMU_MPCC0_MPCC_CONTROL,0xFFFF0461},
        {BC250_REG_DMU_MPC_OUT0_MUX,0x01440000}
    };
    BC250_DEVICE d={0};BC250_INHERITED_PIPE p,q;BC250_DISP_IO io;
    D3DKMDT_VIDEO_SIGNAL_INFO signal={0};unsigned i,bottom,mode,count=RTL_NUMBER_OF(measured);ULONG support;
    PipeFixture(1920,1080,1920,1080);IoOpen(&d,&io,0);
    CHECK(count==21 && count+2==BC250_INHERITED_PIPE_READS);
    for(i=0;i<count;++i)pipeRegisters[measured[i].offset/4]=measured[i].value;
    CHECK(Bc250InheritedPipeRead(&io,&p)==0 && p.Valid==BC250_INHERITED_PIPE_ALL);
    support=Bc250InheritedPipeSupport(&p,1920,1080,7680,1920,1080);
    CHECK((support&BC250_SCALING_BIT(BC250_SCALING_IDENTITY))!=0);
    CHECK(p.MpccBottom==0 && p.MpccControl==0xFFFF0461); // classification leaves the snapshot intact
    d.Post.Width=1920;d.Post.Height=1080;d.Post.Pitch=7680;
    signal.ActiveSize.cx=1920;signal.ActiveSize.cy=1080;
    CHECK(ModesetPrepareInherited(&d,&signal)==STATUS_SUCCESS);
    CHECK((d.Modeset.InheritedSupport&BC250_SCALING_BIT(BC250_SCALING_IDENTITY))!=0);
    // dc/inc/hw/mpc.h: mode 1 passes through the top layer; its bottom selector
    // is unused. Keep BD-122's mode-2 disconnected-bottom requirement and the
    // refusal of mode 0 bypass and mode 3 two-layer blending.
    for(mode=0;mode<4;++mode)for(bottom=0;bottom<16;++bottom) {
        ULONG got;int admitted=mode==1 || (mode==2 && bottom==15);
        q=p;q.MpccBottom=Bc250DispField(MPCC0_MPCC_BOT_SEL__MPCC_BOT_SEL_MASK,bottom);
        q.MpccControl=(p.MpccControl&~MPCC0_MPCC_CONTROL__MPCC_MODE_MASK)|
            Bc250DispField(MPCC0_MPCC_CONTROL__MPCC_MODE_MASK,mode);
        got=Bc250InheritedPipeSupport(&q,1920,1080,7680,1920,1080);
        CHECK(admitted?(got&BC250_SCALING_BIT(BC250_SCALING_IDENTITY))!=0:got==0);
        CHECK(q.MpccBottom==bottom); // classifier must not rewrite the retained evidence
    }
    CHECK(pipeWrites==0 && io.StalledUs==0);
}

// Mutations of a known synthetic scaled tuple. These test the production
// classifier and exact-source prepare function, not a second decoder.
static void InheritedPipeCases(void)
{
    BC250_DEVICE d={0};BC250_INHERITED_PIPE p,q;BC250_DISP_IO io;
    unsigned i;
    static const struct {ULONG offset,value;} bad[] = {
        {BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START,1},
        {BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION,0},
        {BC250_REG_DMU_DSCL0_RECOUT_START,0x3FFF},
        {BC250_REG_DMU_DSCL0_RECOUT_SIZE,0},
        {BC250_REG_DMU_DSCL0_MPC_SIZE,0},
        {BC250_REG_DMU_DSCL0_SCL_MODE,2},
        {BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_CTRL,DSCL0_DSCL_MEM_PWR_CTRL__LUT_MEM_PWR_FORCE_MASK},
        {BC250_REG_DMU_MPCC0_MPCC_TOP_SEL,1},
        {BC250_REG_DMU_MPC_OUT0_MUX,1},
        {BC250_REG_DMU_MPCC0_MPCC_BOT_SEL,1},
        {BC250_REG_DMU_MPCC0_MPCC_CONTROL,3},
        {BC250_REG_DMU_MPCC0_MPCC_CONTROL,0},
        {BC250_REG_DMU_ODM0_OPTC_DATA_SOURCE_SELECT,ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL_MASK},
        {BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO,0},
        {BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO,1},
        {BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG,HUBP0_DCSURF_SURFACE_CONFIG__ROTATION_ANGLE_MASK},
        {BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG,HUBP0_DCSURF_SURFACE_CONFIG__H_MIRROR_EN_MASK},
        {BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG,1},
        {BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,0},
        {BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL,HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK},
        {BC250_REG_DMU_OTG1_OTG_CONTROL,OTG1_OTG_CONTROL__OTG_MASTER_EN_MASK}
    };
    InheritedSelfBottomCases();
    IoOpen(&d,&io,0);
    PipeFixture(1024,768,1920,1080);
    CHECK(Bc250InheritedPipeRead(&io,&p)==0 && p.Valid==BC250_INHERITED_PIPE_ALL);
    CHECK(Bc250InheritedPipeSupport(&p,1024,768,4096,1920,1080)==BC250_SCALING_BIT(BC250_SCALING_STRETCHED));
    // AUTOCAL_MODE=1 derives ratio/phase from viewport and recout. Its manual
    // ratio words are inactive, so the geometry, not those words, names it.
    {
        static const struct {ULONG x,y,w,h,expected;} shapes[]={
            {0,0,1920,1080,BC250_SCALING_STRETCHED},
            {448,156,1024,768,BC250_SCALING_CENTERED},
            {240,0,1440,1080,BC250_SCALING_ASPECT},
            {0,0,1918,1080,BC250_SCALING_CUSTOM}
        };
        unsigned shape,ratios;
        for(shape=0;shape<RTL_NUMBER_OF(shapes);shape++)for(ratios=0;ratios<3;ratios++) {
            BC250_INHERITED_PIPE before;
            q=p;q.Autocal=1;
            q.Shape.RecoutStart=shapes[shape].x|(shapes[shape].y<<16);
            q.Shape.RecoutSize=shapes[shape].w|(shapes[shape].h<<16);
            q.RatioH=q.RatioV=ratios==0?0:ratios==1?0x01000000:0x01000040;
            before=q;
            CHECK(Bc250InheritedPipeSupport(&q,1024,768,4096,1920,1080)==BC250_SCALING_BIT(shapes[shape].expected));
            CHECK(!memcmp(&before,&q,sizeof(q)) && !pipeWrites && !io.StalledUs);
        }
        q=p;q.Autocal=1;q.Shape.RecoutStart=1; // full width no longer fits
        CHECK(!Bc250InheritedPipeSupport(&q,1024,768,4096,1920,1080));
        q=p;q.Autocal=1;q.Shape.RecoutSize=1023|(768ul<<16); // no automatic downscale admission
        CHECK(!Bc250InheritedPipeSupport(&q,1024,768,4096,1920,1080));
    }
    q=p;q.Valid=0;CHECK(!Bc250InheritedPipeSupport(&q,1024,768,4096,1920,1080));
    CHECK(!Bc250InheritedPipeSupport(&p,1024,768,4095,1920,1080));
    CHECK(!Bc250InheritedPipeSupport(&p,0,768,4096,1920,1080));
    CHECK(!Bc250InheritedPipeSupport(&p,1024,768,4096,0,1080));
    for(i=0;i<RTL_NUMBER_OF(bad);i++) {
        PipeFixture(1024,768,1920,1080);pipeRegisters[bad[i].offset/4]=bad[i].value;
        CHECK(Bc250InheritedPipeRead(&io,&p)==0);
        CHECK(!Bc250InheritedPipeSupport(&p,1024,768,4096,1920,1080));
        CHECK(pipeWrites==0 && io.StalledUs==0);
    }
    // A centered, unscaled GOP (bypass) can have stale coefficient ratios.
    PipeFixture(1024,768,1920,1080);
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_START/4]=448|(156ul<<16);
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=1024|(768ul<<16);
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_MODE/4]=0;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=0;
    CHECK(Bc250InheritedPipeRead(&io,&p)==0);
    CHECK(Bc250InheritedPipeSupport(&p,1024,768,4096,1920,1080)==BC250_SCALING_BIT(BC250_SCALING_CENTERED));
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_START/4]=0; // same resolutions, different actual placement
    CHECK(Bc250InheritedPipeRead(&io,&q)==0 && !Bc250InheritedPipeSame(&p,&q));
    CHECK(Bc250InheritedPipeSupport(&q,1024,768,4096,1920,1080)==BC250_SCALING_BIT(BC250_SCALING_CUSTOM));
    q=p;q.Shape.HubpCntl^=HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS_MASK;
    CHECK(Bc250InheritedPipeSame(&p,&q)); // unrelated live status is not a changed transform
    // Aspect ratio is determined by recout as well as the independently read ratios.
    PipeFixture(1024,768,1920,1080);
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_START/4]=240;
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=1440|(1080ul<<16);
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=(ULONG)((1024ull<<24)/1440);
    CHECK(Bc250InheritedPipeRead(&io,&p)==0);
    CHECK(Bc250InheritedPipeSupport(&p,1024,768,4096,1920,1080)==BC250_SCALING_BIT(BC250_SCALING_ASPECT));
    // Custom classification grants no register programming path.
    {BC250_SCALER_PLAN plan;CHECK(Bc250ScalerPlan(1024,768,1920,1080,BC250_SCALING_CUSTOM,&plan)<0);}
    CHECK(pipeWrites==0 && io.StalledUs==0);
    // Actual mode-list admission and commit, including no-preference, smaller
    // source, pinned wrong scaling, custom, repeat commit and the D3 interval.
    d.Post.Width=1024;d.Post.Height=768;d.Post.Pitch=4096;
    d.Modeset.InheritedFixed=TRUE;d.Modeset.InheritedSupport=BC250_SCALING_BIT(BC250_SCALING_STRETCHED);
    CHECK(ModesetScalingSupport(&d,1024,768)==BC250_SCALING_BIT(BC250_SCALING_STRETCHED));
    CHECK(!ModesetScalingSupport(&d,800,600));
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_COUNT)==STATUS_SUCCESS);
    CHECK(d.Modeset.Scaling==BC250_SCALING_STRETCHED && !d.Modeset.Lock);
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_STRETCHED)==STATUS_SUCCESS);
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_IDENTITY)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_ASPECT)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    CHECK(ModesetCommit(&d,800,600,BC250_SCALING_STRETCHED)==STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE);
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_COUNT+1)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    d.Modeset.Suspended=TRUE;
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_STRETCHED)==STATUS_SUCCESS);
    d.Modeset.InheritedSupport=BC250_SCALING_BIT(BC250_SCALING_CUSTOM);
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_COUNT)==STATUS_SUCCESS);
    CHECK(d.Modeset.Scaling==BC250_SCALING_CUSTOM);
    CHECK(!d.Modeset.PipeChanged && !commitTransactions && !commitPrograms && !pipeWrites);
    d.Modeset.InheritedSupport=0;
    CHECK(ModesetCommit(&d,1024,768,BC250_SCALING_CUSTOM)==STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE);
}
