// BD-169: a start that 0.7.216.63 completed must still complete. 0.7.216.64 sent
// every inherited timing above 300 MHz down the inherited route (modeset.c) and
// let a connector refusal fail the adapter start (pnp.c). Both together turn an
// equal-size board whose pipe reads no support into Code 43, and even a board
// whose pipe classifies loses its source mode list.
//
// The first group carries the register words GitHub #40's board logged on
// 0.7.216.61 (Acer KG271U on DP, 2560x1440, pitch 10240, MPCC mode 1 with a
// disconnected-looking bottom selector, MPC_OUT0_MUX 0x01440000, OTG1 not
// master, tiling 0x80). Inference, marked as such: the kept log carries no
// totals and no AUTOCAL/DSCL_CONTROL (those two reads are newer than that
// build), so the totals here are the CVT-RB 120 Hz tuple of that active size
// and AUTOCAL is manual, the firmware's ordinary state.
static void Bd169Field40Pipe(void)
{
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_BOT_SEL/4]=0x00000000;
    pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_CONTROL/4]=0xFFFF0461;
    pipeRegisters[BC250_REG_DMU_MPC_OUT0_MUX/4]=0x01440000;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG/4]=0x00000080;
    pipeRegisters[BC250_REG_DMU_OTG1_OTG_CONTROL/4]=0x80000310;
    pipeRegisters[BC250_REG_DMU_HUBP0_DCHUBP_CNTL/4]=0x000E1000;
}

// The programmable connector's own prerequisites, so that the route selector
// under test decides on the inherited tuple and not on a missing EDID.
static void Bd169Programmable(BC250_DEVICE* d)
{
    d->Modeset.Requested=BC250_DISPLAY_MODES_SCALED;d->Modeset.Level=BC250_DISPLAY_MODES_SCALED;
    d->Modeset.EdidValid=TRUE;d->Modeset.DpcdValid=TRUE;d->DcnWriteEnabled=TRUE;
    d->Dxgk.DxgkCbSynchronizeExecution=d;d->Dxgk.DxgkCbQueueDpc=d;d->Dxgk.DxgkCbIndicateChildStatus=d;
}

static void Bd169ModeList(BC250_DEVICE* d,ULONG w,ULONG h,ULONG sw,ULONG sh)
{
    d->Modeset.Modes.Count=2;
    d->Modeset.Modes.Modes[0].Width=w;d->Modeset.Modes.Modes[0].Height=h;
    d->Modeset.Modes.Modes[1].Width=sw;d->Modeset.Modes.Modes[1].Height=sh;
}

static void Bd169Cases(void)
{
    // Equal source and target above the Plan() bound: the firmware applied no
    // transform, the scaler needs no output programming, and the start holds.
    static const struct { ULONG w,h,ht,vt,pixel; } above[]={
        {2560,1440,2720,1481,483326400},    // #40, 120 Hz (totals inferred)
        {2560,1440,2720,1492,304250000}     // BD-144, 75 Hz; was the 4th BD-155 case
    };
    unsigned i;
    for(i=0;i<RTL_NUMBER_OF(above);i++) {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        Mode(&d,above[i].w,above[i].h,above[i].ht,above[i].vt,above[i].pixel);
        if(i==0)Bd169Field40Pipe();
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS);
        CHECK(signal.PixelRate==above[i].pixel && signal.ActiveSize.cx==above[i].w);
        // The start gate: no transform to preserve, so no fixed single-mode
        // route and no inherited-pipe status in the start's result.
        CHECK(!d.Modeset.InheritedFixed && d.Modeset.InheritedPipeStatus==STATUS_SUCCESS);
        CHECK(d.Modeset.InheritedSupport&BC250_SCALING_BIT(BC250_SCALING_IDENTITY));
        CHECK(pipeWrites==0);
        Bd169Programmable(&d);
        Bd169ModeList(&d,above[i].w,above[i].h,1920,1080);
        // Plan() refuses this output timing, so ConnectorStart refuses: the
        // start continues and the firmware's stream stays on the wire.
        CHECK(!ConnectorNeedsInherited(&d));
        CHECK(ConnectorRefusalKeepsDisplay(&d));
        // And the source mode list survives: scaled modes are the scaler's, not
        // the connector's. 0.7.216.64 answered 0 for every non-native size.
        CHECK(ModesetScalingSupport(&d,above[i].w,above[i].h)!=0);
        CHECK(ModesetScalingSupport(&d,1920,1080)!=0);
        CHECK(pipeWrites==0);
    }
    // The same board when the pipe classifies as nothing we name: MPCC mode 3
    // (two-layer blending) stands in for #40's own 0.7.216.61 verdict, whose
    // bottom-selector gate rejected MPCC mode 1 (`support 0 status C00000BB`).
    // The published tuple is the POST tuple either way, so the start holds and
    // the identity transform comes from that tuple, not from the registers.
    {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        Mode(&d,2560,1440,2720,1481,483326400);
        Bd169Field40Pipe();
        pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_CONTROL/4]=0xFFFF0463;
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS);
        CHECK(!d.Modeset.InheritedFixed && d.Modeset.InheritedSupport==0);
        CHECK(d.Modeset.InheritedPipeStatus==STATUS_NOT_SUPPORTED && pipeWrites==0);
        Bd169Programmable(&d);
        Bd169ModeList(&d,2560,1440,1920,1080);
        CHECK(!ConnectorNeedsInherited(&d));
        CHECK(ConnectorRefusalKeepsDisplay(&d));
        CHECK(ModesetScalingSupport(&d,2560,1440)!=0 && ModesetScalingSupport(&d,1920,1080)!=0);
        // Nothing to present is still a failed start: no signal, or a mode list
        // without the POST size.
        d.InheritedSignalValid=FALSE;CHECK(!ConnectorRefusalKeepsDisplay(&d));
        d.InheritedSignalValid=TRUE;Bd169ModeList(&d,1920,1080,1280,720);
        CHECK(!ConnectorRefusalKeepsDisplay(&d));
        d.Modeset.Modes.Count=0;CHECK(!ConnectorRefusalKeepsDisplay(&d));
    }
    // #38 unchanged: a scaled firmware tuple is the inherited route, its refusal
    // leaves nothing to present, and the start fails there (BD-155).
    {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        Mode(&d,2560,1440,2720,1492,304250000);
        d.Post.Width=800;d.Post.Height=600;d.Post.Pitch=3328;
        PipeFixture(d.Post.Width,d.Post.Height,2560,1440);
        pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=d.Post.Pitch/4-1;
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS);
        CHECK(d.Modeset.InheritedFixed && d.Modeset.InheritedSupport!=0 && pipeWrites==0);
        Bd169Programmable(&d);
        Bd169ModeList(&d,d.Post.Width,d.Post.Height,640,480);
        CHECK(ConnectorNeedsInherited(&d));
        CHECK(!ConnectorRefusalKeepsDisplay(&d));
        CHECK(ConnectorStartInherited(&d)==STATUS_SUCCESS && d.Connector.StartAccepted==1);
        CHECK(d.Modeset.Modes.Count==1 && d.Modeset.Modes.Modes[0].Width==d.Post.Width);
        d.Modeset.InheritedSupport=0;
        CHECK(ConnectorStartInherited(&d)==STATUS_NOT_SUPPORTED && !ConnectorRefusalKeepsDisplay(&d));
        // A scaled tuple the pipe does not describe fails the start itself, in
        // ModesetPrepareInherited, before any connector decision.
        PipeFixture(d.Post.Width,d.Post.Height,2560,1440);
        pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=d.Post.Pitch/4-1;
        pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_CONTROL/4]=3;
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_NOT_SUPPORTED);
        CHECK(d.Modeset.InheritedFixed && d.Modeset.InheritedSupport==0 && pipeWrites==0);
    }
    // Unit A's own native start, below the bound: unchanged by all of this.
    {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        Mode(&d,1920,1080,2200,1125,148500000);
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS && !d.Modeset.InheritedFixed);
        CHECK(d.Modeset.InheritedSupport&BC250_SCALING_BIT(BC250_SCALING_IDENTITY));
        Bd169Programmable(&d);
        Bd169ModeList(&d,1920,1080,1280,720);
        CHECK(!ConnectorNeedsInherited(&d) && ConnectorRefusalKeepsDisplay(&d));
        CHECK(ModesetScalingSupport(&d,1280,720)!=0 && pipeWrites==0);
    }
}
