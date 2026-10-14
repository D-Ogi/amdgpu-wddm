// BD-169: a start that 0.7.216.63 completed with a validated pipe must still
// complete. 0.7.216.64 sent every inherited timing above 300 MHz down the fixed
// inherited route (modeset.c) and let a connector refusal fail the adapter start
// (pnp.c). Together they publish one source mode instead of fifteen on a board
// whose pipe classifies, and fail the start outright when it does not.
//
// What the start may keep is one pipe: stable, validated, identity, with only
// programmable connector admission missing. An unreadable pipe, a moving pipe,
// an unhandled fetch layout and a refusal whose own cleanup failed are not that,
// and each still fails the start. Equal source and target sizes establish none
// of it, and a mode-list entry is not a contract.
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
// under test decides on the inherited tuple and not on a missing EDID, and the
// refusal it will meet is a planning one whose cleanup succeeded.
static void Bd169Programmable(BC250_DEVICE* d)
{
    d->Modeset.Requested=BC250_DISPLAY_MODES_SCALED;d->Modeset.Level=BC250_DISPLAY_MODES_SCALED;
    d->Modeset.EdidValid=TRUE;d->Modeset.DpcdValid=TRUE;d->DcnWriteEnabled=TRUE;
    d->Dxgk.DxgkCbSynchronizeExecution=d;d->Dxgk.DxgkCbQueueDpc=d;d->Dxgk.DxgkCbIndicateChildStatus=d;
    d->Connector.StartPreserved=1;
}

static void Bd169ModeList(BC250_DEVICE* d,ULONG w,ULONG h,ULONG sw,ULONG sh)
{
    d->Modeset.Modes.Count=2;
    d->Modeset.Modes.Modes[0].Width=w;d->Modeset.Modes.Modes[0].Height=h;
    d->Modeset.Modes.Modes[1].Width=sw;d->Modeset.Modes.Modes[1].Height=sh;
}

static void Bd169Cases(void)
{
    // Equal source and target above the Plan() bound, pipe validated as the
    // identity transform: the firmware applied none, the scaler needs no output
    // programming, and the start holds with its own source mode list.
    static const struct { ULONG w,h,ht,vt,pixel; } above[]={
        {2560,1440,2720,1481,483326400},    // #40's class, 120 Hz (totals inferred)
        {2560,1440,2720,1492,304250000}      // BD-144, 75 Hz; was the 4th BD-155 case
    };
    unsigned i;
    testConnectorEnabled=FALSE;             // the static path has no enabled connector
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
        // A refusal that wrote and could not restore it keeps no display, even
        // with this pipe: the hardware state is then unknown.
        d.Connector.StartPreserved=0;
        CHECK(!ConnectorRefusalKeepsDisplay(&d));
        d.Connector.StartPreserved=1;
        CHECK(ConnectorRefusalKeepsDisplay(&d));
        // The D0 of that start verifies the retained output timing, which this
        // driver did not program. Unchanged registers pass.
        CHECK(ModesetValidateInherited(&d)==STATUS_SUCCESS && pipeWrites==0);
        CHECK(DisplayValidateInheritedSignal(&d)==STATUS_SUCCESS);
    }
    // The same board when the pipe classifies as nothing we name: MPCC mode 3
    // (two-layer blending) stands in for any unhandled fetch or ownership
    // layout. The source size is the POST size and the mode list still holds
    // it, and neither admits the pipe: the start fails at the connector gate.
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
        CHECK(!ConnectorRefusalKeepsDisplay(&d));
        // Nothing to present is a failed start for the same reason: no signal,
        // or a mode list without the POST size.
        d.InheritedSignalValid=FALSE;CHECK(!ConnectorRefusalKeepsDisplay(&d));
        d.InheritedSignalValid=TRUE;Bd169ModeList(&d,1920,1080,1280,720);
        CHECK(!ConnectorRefusalKeepsDisplay(&d));
        d.Modeset.Modes.Count=0;CHECK(!ConnectorRefusalKeepsDisplay(&d));
    }
    // A pipe we cannot read at all, and one that moves between the two reads.
    // The start's first gate still passes, because the POST tuple describes an
    // equal-size source; what neither grants is a pipe the driver may own, so
    // the connector refusal fails the start.
    for(i=0;i<2;i++) {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        Mode(&d,2560,1440,2720,1481,483326400);
        Bd169Field40Pipe();
        if(i==0)pipeReadFailure=1;else pipeTear=1;
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS);
        CHECK(!d.Modeset.InheritedFixed && d.Modeset.InheritedSupport==0);
        CHECK(d.Modeset.InheritedPipeStatus==(i==0?STATUS_ACCESS_DENIED:STATUS_DEVICE_BUSY));
        CHECK(pipeWrites==0);
        Bd169Programmable(&d);
        Bd169ModeList(&d,2560,1440,1920,1080);
        CHECK(!ConnectorNeedsInherited(&d) && !ConnectorRefusalKeepsDisplay(&d));
        // The same two failures on a scaled tuple fail the start one gate
        // earlier, in ModesetPrepareInherited (#38).
        {
            BC250_DEVICE s={0};D3DKMDT_VIDEO_SIGNAL_INFO scaled;
            Mode(&s,2560,1440,2720,1492,304250000);
            s.Post.Width=800;s.Post.Height=600;s.Post.Pitch=3328;
            PipeFixture(s.Post.Width,s.Post.Height,2560,1440);
            pipeRegisters[BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH/4]=s.Post.Pitch/4-1;
            if(i==0)pipeReadFailure=1;else pipeTear=1;
            CHECK(TestFillSignalInfo(&s,&scaled)==(i==0?STATUS_ACCESS_DENIED:STATUS_DEVICE_BUSY));
            CHECK(s.Modeset.InheritedFixed && s.Modeset.InheritedSupport==0 && pipeWrites==0);
        }
    }
    // D0 of the static path: the retained output timing is verified because this
    // driver did not program it, and the boot scaler snapshot is not, because a
    // committed runtime source scale differs from it legitimately.
    for(i=0;i<5;i++) {
        BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO signal;
        NTSTATUS expected;
        Mode(&d,2560,1440,2720,1481,483326400);
        Bd169Field40Pipe();
        CHECK(TestFillSignalInfo(&d,&signal)==STATUS_SUCCESS && !d.Modeset.InheritedFixed);
        Bd169Programmable(&d);
        Bd169ModeList(&d,2560,1440,1920,1080);
        switch(i) {
        case 0: expected=STATUS_SUCCESS; break;                                 // unchanged
        case 1: values[TimingPhase]+=1000000u; expected=STATUS_DEVICE_BUSY; break;   // another clock
        case 2: values[TimingHTotal]+=1; expected=STATUS_DEVICE_BUSY; break;         // another total
        case 3: readFailure=reads+1; expected=STATUS_ACCESS_DENIED; break;           // unreadable
        default:                                                                     // the scaler moved
            // A later source-scale commit leaves the boot snapshot behind. The
            // timing is the question on this path, so D0 still passes.
            pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=1280ul|(720ul<<16);
            expected=STATUS_SUCCESS; break;
        }
        CHECK(ModesetValidateInherited(&d)==expected);
        CHECK(pipeWrites==0);
        // Two gates of that check, both unchanged by BD-169: a connector that
        // programmed its own mode has its own resume, and display-only never
        // measured a timing to verify.
        if(i==1) {
            testConnectorEnabled=TRUE;
            CHECK(ModesetValidateInherited(&d)==STATUS_SUCCESS);
            testConnectorEnabled=FALSE;
            d.FullWddm=0;
            CHECK(ModesetValidateInherited(&d)==STATUS_SUCCESS);
            d.FullWddm=1;
            CHECK(ModesetValidateInherited(&d)==STATUS_DEVICE_BUSY);
        }
    }
    // #38 unchanged: a scaled firmware tuple is the inherited route, its refusal
    // leaves nothing to present, and the start fails there (BD-155). The field
    // record of that board reads support 0x10 (CUSTOM) with status SUCCESS, so
    // it is not a support-0 case; the fixture's layout classifies on its own.
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
        // Its D0 keeps both halves: the timing and the boot scaler snapshot.
        CHECK(ModesetValidateInherited(&d)==STATUS_SUCCESS && pipeWrites==0);
        pipeRegisters[BC250_REG_DMU_DSCL0_RECOUT_SIZE/4]=1280ul|(720ul<<16);
        CHECK(ModesetValidateInherited(&d)==STATUS_DEVICE_BUSY);
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
        CHECK(ModesetValidateInherited(&d)==STATUS_SUCCESS && pipeWrites==0);
    }
    // ---- the two cases of the BD-169 review, as supplied ------------------------------------------------------
    // Synthetic registers, not a field or hardware replay. The reviewer's first
    // case needs the static path's disabled connector, which the fixture global
    // carries, so this sets it; the assertions are the reviewer's.
    {
        BC250_DEVICE probe={0}; D3DKMDT_VIDEO_SIGNAL_INFO signal;
        NTSTATUS direct, through, fixedControl;
        Mode(&probe,2560,1440,2720,1481,483326400);
        testConnectorEnabled=FALSE;
        CHECK(TestFillSignalInfo(&probe,&signal)==STATUS_SUCCESS);
        CHECK(!probe.Modeset.InheritedFixed);
        // Same POST geometry and pointer, different running clock after retained D3.
        values[TimingPhase]+=1000000u;
        direct=DisplayValidateInheritedSignal(&probe);
        through=ModesetValidateInherited(&probe);
        probe.Modeset.InheritedFixed=TRUE;
        fixedControl=ModesetValidateInherited(&probe);
        CHECK(direct==STATUS_DEVICE_BUSY && fixedControl==STATUS_DEVICE_BUSY);
        CHECK(through==STATUS_DEVICE_BUSY); // Required retained timing rejection.
    }
    {
        BC250_DEVICE probe={0}; D3DKMDT_VIDEO_SIGNAL_INFO signal;
        ULONG level=0, shapeVerdict;
        Mode(&probe,2560,1440,2720,1481,483326400);
        pipeRegisters[BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG/4] |=
            1ul << HUBP0_DCSURF_TILING_CONFIG__SW_MODE__SHIFT;
        CHECK(TestFillSignalInfo(&probe,&signal)==STATUS_SUCCESS);
        CHECK(probe.Modeset.InheritedSupport==0 && probe.Modeset.InheritedPipeStatus==STATUS_NOT_SUPPORTED);
        shapeVerdict=Bc250PipeShapeCheck(&probe.Modeset.InheritedPipe.Shape,2560,1440,&level);
        Bd169Programmable(&probe);
        probe.Modeset.Level=level;
        Bd169ModeList(&probe,2560,1440,1920,1080);
        CHECK(level==BC250_DISPLAY_MODES_SCALED); // Old shape gate does not validate surface layout.
        CHECK(!ConnectorRefusalKeepsDisplay(&probe)); // Required refusal of unhandled fetch layout.
        CHECK(shapeVerdict<BC250_PIPE_VERDICT_COUNT);
    }
}
