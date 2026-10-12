// The 21 recorded words are the tester.29 #38 start tuple. AUTOCAL and
// DSCL_CONTROL were not recorded on that board. The manual extension 0x100
// was measured on unit A at 1920x1080, not on the tester's hardware. Automatic
// mode 0x1 and the boundary alternatives remain synthetic extensions.
static void Field38Fixture(ULONG autocal, ULONG boundary)
{
    PipeFixture(800,600,2560,1440);
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION/4]=0x02580320;
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=0x05A00A00;
    pipeRegisters[BC250_REG_DMU_DSCL0_MPC_SIZE/4]=0x05A00A00;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_MODE/4]=1;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCHUBP_CNTL/4]=0x000F1002;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=0x01000000;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO/4]=0x01000000;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL/4]=0x11;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG/4]=8;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG/4]=0x80;
    pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=0x33F;
    pipeRegisters[BC250_REG_DMU_OTG1_OTG_CONTROL/4]=0x80000310;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_BOT_SEL/4]=0;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_CONTROL/4]=0xFFFF0461;
    pipeRegisters[BC250_REG_DMU_MPC_OUT0_MUX/4]=0x01440000;
    pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_AUTOCAL/4]=autocal;
    pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_CONTROL/4]=boundary;
}

// All 23 pipe words from unit A, .49 HDMI 1920x1080, 2026-10-12 14:27:50Z.
// This is a separate measured control, not the field board's missing words.
static void UnitA49Fixture(void)
{
    PipeFixture(1920,1080,1920,1080);
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START/4]=0;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION/4]=0x04380780;
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_START/4]=0;
    pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=0x04380780;
    pipeRegisters[BC250_REG_DMU_DSCL0_MPC_SIZE/4]=0x04380780;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_MODE/4]=1;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=0x01000000;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO/4]=0x01000000;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_TOP_SEL/4]=0;
    pipeRegisters[BC250_REG_DMU_ODM0_OPTC_DATA_SOURCE_SELECT/4]=0;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCHUBP_CNTL/4]=0x000F100A;
    pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_CTRL/4]=0;
    pipeRegisters[BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL/4]=0;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG/4]=8;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG/4]=0x80;
    pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=0x77F;
    pipeRegisters[BC250_REG_DMU_OTG1_OTG_CONTROL/4]=0x80000310;
    pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL/4]=0;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_BOT_SEL/4]=0;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_CONTROL/4]=0xFFFF0461;
    pipeRegisters[BC250_REG_DMU_MPC_OUT0_MUX/4]=0x01440000;
    pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_AUTOCAL/4]=0x100;
    pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_CONTROL/4]=0;
}

static void InheritedField38Cases(void)
{
    BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
    BC250_INHERITED_PIPE p,q;BC250_DISP_IO io;
    unsigned autocal,boundary,bottom,n,count,id;
    const unsigned long custom=BC250_SCALING_BIT(BC250_SCALING_CUSTOM);
    const unsigned long native=BC250_SCALING_BIT(BC250_SCALING_IDENTITY) |
        BC250_SCALING_BIT(BC250_SCALING_CENTERED) | BC250_SCALING_BIT(BC250_SCALING_STRETCHED) |
        BC250_SCALING_BIT(BC250_SCALING_ASPECT);
    IoOpen(&d,&io,0);
    for(autocal=0;autocal<=1;autocal++)for(boundary=0;boundary<=1;boundary++) {
        Mode(&d,2560,1440,2720,1481,241500000);
        d.Post.Width=800;d.Post.Height=600;d.Post.Pitch=3328;
        Field38Fixture(autocal?1ul:0x100ul,boundary);
        CHECK(Bc250InheritedPipeRead(&io,&p)==0);
        CHECK(p.Valid==BC250_INHERITED_PIPE_ALL);
        CHECK(Bc250InheritedPipeSupport(&p,800,600,3328,2560,1440)==custom);
        q=p;q.Valid=(1ul<<21)-1; // original record has no automatic-mode observation
        CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        CHECK(TestFillSignalInfo(&d,&signal)==0 && d.InheritedSignalValid);
        CHECK(signal.PixelRate==241500000 && signal.ActiveSize.cx==2560 && signal.ActiveSize.cy==1440);
        CHECK(d.Modeset.InheritedFixed && d.Modeset.InheritedSupport==custom);
        CHECK(ModesetScalingSupport(&d,800,600)==custom && !ModesetScalingSupport(&d,1024,768));
        CHECK(ModesetCommit(&d,800,600,BC250_SCALING_COUNT)==0 && d.Modeset.Scaling==BC250_SCALING_CUSTOM);
        CHECK(ModesetCommit(&d,800,600,BC250_SCALING_STRETCHED)==STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
        {
            D3DKMDT_VIDPN_SOURCE_MODE mode={0};D3DKMDT_VIDPN_PRESENT_PATH path={0};
            DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH update={0};
            mode.Type=D3DKMDT_RMT_GRAPHICS;
            mode.Format.Graphics.PrimSurfSize.cx=800;mode.Format.Graphics.PrimSurfSize.cy=600;
            mode.Format.Graphics.VisibleRegionSize=mode.Format.Graphics.PrimSurfSize;
            mode.Format.Graphics.Stride=3328;mode.Format.Graphics.PixelFormat=21;
            path.ContentTransformation.Scaling=D3DKMDT_VPPS_CUSTOM;
            path.ContentTransformation.Rotation=D3DKMDT_VPPR_IDENTITY;
            CHECK(DisplayInheritedModeMatches(&d,&mode,&path,&signal));
            update.VidPnPresentPathInfo=path;
            CHECK(Bc250UpdateActiveVidPnPresentPath(&d,&update)==0);
            path.ContentTransformation.Scaling=D3DKMDT_VPPS_STRETCHED;
            CHECK(!DisplayInheritedModeMatches(&d,&mode,&path,&signal));
        }
        CHECK(ModesetValidateInherited(&d)==0);
        pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_AUTOCAL/4]^=1;
        CHECK(ModesetValidateInherited(&d)==STATUS_DEVICE_BUSY);
        pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_AUTOCAL/4]^=1;
        pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_CONTROL/4]^=1;
        CHECK(ModesetValidateInherited(&d)==STATUS_DEVICE_BUSY);
        CHECK(pipeWrites==0 && commitPrograms==0 && commitTransactions==0);
        for(bottom=0;bottom<16;bottom++) {
            q=p;q.MpccBottom=bottom;
            CHECK(Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440)==custom);
            q.MpccControl=(q.MpccControl&~MPCC0_MPCC_CONTROL__MPCC_MODE_MASK)|2;
            CHECK(Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440)==(bottom==15?custom:0));
        }
        q=p;q.MpccControl=3;CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        q=p;q.MpccControl=0;CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        q=p;q.Autocal=2;CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        q=p;q.Autocal=3;CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        // Pipe fields are inactive in manual mode. For active automatic
        // calculation, retain the refusal for any unverified nonzero field.
        for(count=0;count<4;count++)for(id=0;id<4;id++) {
            q=p;q.Autocal=autocal |
                (count<<DSCL0_DSCL_AUTOCAL__AUTOCAL_NUM_PIPE__SHIFT) |
                (id<<DSCL0_DSCL_AUTOCAL__AUTOCAL_PIPE_ID__SHIFT);
            CHECK(Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440)==
                ((autocal && (count || id))?0:custom));
        }
        q=p;q.Shape.SclMode=0;CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        q=p;q.Shape.MemPwrCtrl=DSCL0_DSCL_MEM_PWR_CTRL__LUT_MEM_PWR_FORCE_MASK;
        CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        q=p;q.Shape.RecoutSize=640|(480ul<<16);
        CHECK(!Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440));
        q=p;q.RatioH=0;q.RatioV=0;
        CHECK(Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440)==(autocal?custom:0));
        q=p;q.RatioH=0x01000040;
        CHECK(Bc250InheritedPipeSupport(&q,800,600,3328,2560,1440)==(autocal?custom:0));
        CHECK(pipeWrites==0 && io.StalledUs==0);
    }
    // A real read failure in either snapshot, including each new register,
    // stays a failure. It is not an inferred automatic-mode or zero word.
    for(n=1;n<=2*BC250_INHERITED_PIPE_READS;n++) {
        Field38Fixture(1,0);pipeReadFailure=n;
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_ACCESS_DENIED);
        CHECK(!d.InheritedSignalValid && !d.Modeset.InheritedSupport && pipeWrites==0);
    }
    Field38Fixture(1,0);pipeTear=1;
    CHECK(TestFillSignalInfo(&d,&signal)==STATUS_DEVICE_BUSY && !d.InheritedSignalValid && !pipeWrites);
    // Unit A's captured direct-DP shape uses the same passthrough/bottom=0
    // combination at 1920x1200. The HDMI controls retain their original
    // timing/transport model from Adapter(). The smaller-source control
    // is a synthetic scaled-source control, not a captured 1024x768 scanout.
    Mode(&d,1920,1200,2080,1235,154000000);
    pipeRegisters[BC250_REG_DMU_HUBP0_DCHUBP_CNTL/4]=0x000F1002;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG/4]=0x80;
    pipeRegisters[BC250_REG_DMU_OTG1_OTG_CONTROL/4]=0x80000310;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_BOT_SEL/4]=0;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_CONTROL/4]=0xFFFF0461;
    pipeRegisters[BC250_REG_DMU_MPC_OUT0_MUX/4]=0x01440000;
    CHECK(TestFillSignalInfo(&d,&signal)==0 && !d.Modeset.InheritedFixed);
    CHECK(d.Modeset.InheritedSupport&BC250_SCALING_BIT(BC250_SCALING_IDENTITY));
    CHECK(signal.PixelRate==154000000 && !pipeWrites);
    Adapter(&d);UnitA49Fixture();
    CHECK(Bc250InheritedPipeRead(&io,&p)==0 && p.Valid==BC250_INHERITED_PIPE_ALL && p.Autocal==0x100);
    CHECK(Bc250InheritedPipeSupport(&p,1920,1080,7680,1920,1080)==native);
    CHECK(TestFillSignalInfo(&d,&signal)==0 && d.InheritedTmds && !d.Modeset.InheritedFixed && !pipeWrites);
    CHECK(d.Modeset.InheritedSupport==native && ModesetValidateInherited(&d)==0);
    // A synthetic equal-size automatic variant must not pin Custom or lose
    // the native mode because its manual ratio registers contain stale data.
    for(n=0;n<2;n++) {
        Adapter(&d);UnitA49Fixture();
        pipeRegisters[BC250_REG_DMU_DSCL0_DSCL_AUTOCAL/4]=1;
        pipeRegisters[BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO/4]=n?0x02000000:0;
        pipeRegisters[BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO/4]=n?0:0x02000000;
        CHECK(Bc250InheritedPipeRead(&io,&p)==0);
        CHECK(Bc250InheritedPipeSupport(&p,1920,1080,7680,1920,1080)==native);
        CHECK(TestFillSignalInfo(&d,&signal)==0 && d.InheritedTmds && !d.Modeset.InheritedFixed);
        CHECK(d.Modeset.InheritedSupport==native && ModesetValidateInherited(&d)==0 && !pipeWrites);
        q=p;q.Autocal=0x101;CHECK(!Bc250InheritedPipeSupport(&q,1920,1080,7680,1920,1080));
        q=p;q.Autocal=0x1001;CHECK(!Bc250InheritedPipeSupport(&q,1920,1080,7680,1920,1080));
        q=p;q.Autocal=0x100;CHECK(!Bc250InheritedPipeSupport(&q,1920,1080,7680,1920,1080));
    }
    Adapter(&d);d.Post.Width=1024;d.Post.Height=768;d.Post.Pitch=4096;
    PipeFixture(1024,768,1920,1080);
    CHECK(TestFillSignalInfo(&d,&signal)==0 && d.InheritedTmds && d.Modeset.InheritedFixed);
    CHECK(d.Modeset.InheritedSupport==BC250_SCALING_BIT(BC250_SCALING_STRETCHED) && !pipeWrites);
    puts("field38: field extensions, measured unit A 0x100, native automatic, custom-only and negative controls passed");
}
