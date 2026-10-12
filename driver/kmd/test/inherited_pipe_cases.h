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
    IoOpen(&d,&io,0);
    PipeFixture(1024,768,1920,1080);
    CHECK(Bc250InheritedPipeRead(&io,&p)==0 && p.Valid==BC250_INHERITED_PIPE_ALL);
    CHECK(Bc250InheritedPipeSupport(&p,1024,768,4096,1920,1080)==BC250_SCALING_BIT(BC250_SCALING_STRETCHED));
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
