
static void StopPhases(BC250_DEVICE*d)
{
#ifdef HAS_PHASED_STOP
 GfxPrepareStop(d);PspStop(d);GartPrepareStop(d);GfxStop(d);GartStop(d);
#else
 GfxStop(d);PspStop(d);GartStop(d);
#endif
}
int main(void)
{
 unsigned mask;
#ifdef HAS_SDMA_RELOAD
 {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;int result;
  lookupFail=sequenceFail=undoFail=mappingFail=0;finiQuiet=1;reloadQuiesceFail=1;
  mappingCalls=rlcStops=rlcLive=phaseOrderBad=0;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;d.IhQuiet=1;modelAdev.backend=&gfx.Sequence;
  result=HaltForMappingRetirement(&d,&gfx,&modelAdev,&undo);
  check(!result && undo==-62 && mappingCalls==0,"failed quiescence blocks mapping retirement",0);
  check(rlcStops==1 && d.Gfx==&gfx && gfx.SetUp,"failed quiescence retains owner and attempts RLC stop",0);
  reloadQuiesceFail=0;
 }
#endif

#ifdef HAS_MAPPING_RETIREMENT
 for(mask=0;mask<4;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;int result;
  int oldRelease=sequenceReleaseCalls,oldTeardown=teardownCalls;
  lookupFail=sequenceFail=0;finiQuiet=1;undoFail=(mask>>1)&1;mappingFail=mask&1;
  mappingCalls=rlcStops=rlcLive=phaseOrderBad=0;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;d.IhQuiet=1;modelAdev.backend=&gfx.Sequence;
  result=HaltForMappingRetirement(&d,&gfx,&modelAdev,&undo);
  check(result==(!undoFail&&!mappingFail),"mapping retirement contributes to final verdict",mask);
  check(mappingCalls==!undoFail && rlcStops==1 && !rlcLive && !phaseOrderBad,
        "mapping retirement after CP halt while RLC live, RLC stopped afterwards",mask);
  check(d.Gfx==&gfx && gfx.SetUp && gfx.StagesDone==8 &&
        sequenceReleaseCalls==oldRelease && teardownCalls==oldTeardown,
        "all backing and owners retained through mapping/RLC phases",mask);
 }
 mappingFail=0;
#endif
 for(mask=0;mask<256;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};BC250_PSP psp={0};BC250_GART gart={0};
  int expectedGfx,expectedPsp,expectedRestore,expectedFree,oldUnloads,oldRestores,oldFrees;
  lookupFail=(mask>>0)&1;finiQuiet=!((mask>>1)&1);undoFail=(mask>>2)&1;
  sequenceFail=(mask>>3)&1;unloadFail=(mask>>4)&1;restoreFail=(mask>>5)&1;
  d.IhQuiet=!((mask>>6)&1);disableFail=(mask>>7)&1;
  expectedGfx=!lookupFail&&finiQuiet&&!undoFail&&!sequenceFail;
  expectedPsp=expectedGfx&&!unloadFail;
  expectedRestore=expectedGfx&&expectedPsp&&d.IhQuiet;
#ifdef HAS_PHASED_STOP
  expectedRestore=expectedRestore&&!disableFail;
#endif
  expectedFree=expectedRestore&&!restoreFail;
  locks=regions=unloadCalls=restoreCalls=dummyFrees=viewUnmaps=0;
  memset(poolFrees,0,sizeof(poolFrees));memoryRelease=-1;sequenceRelease=0;disableCalls=0;
  gfx.StagesDone=8;gfx.SetUp=1;psp.RingUp=psp.TmrUp=1;psp.Pages=&psp;
  gart.Enabled=1;gart.DummyPage=&gart;
  d.Gfx=&gfx;d.Psp=&psp;d.Gart=&gart;d.GfxStopQuiet=d.PspStopQuiet=1;
  StopPhases(&d);
  check(d.GfxStopQuiet==expectedGfx,"GFX result includes halt, undo and sequence fault",mask);
#ifdef HAS_PHASED_STOP
  check(sequenceRelease==expectedRestore,"GFX release waits for PSP and GART hardware retirement",mask);
  check(memoryRelease==(expectedRestore?1:-1),"memory destruction only after all hardware phases",mask);
  check(disableCalls==(expectedGfx&&expectedPsp&&d.IhQuiet),"translation disable follows retired consumers",mask);
#else
  check(sequenceRelease==expectedGfx,"Fini releases sequence pages only after clean halt and undo",mask);
  check(memoryRelease==(expectedGfx&&d.IhQuiet),"GTT release needs engine and IH retirement",mask);
#endif
  check(unloadCalls==expectedGfx,"PSP unload never follows unconfirmed GFX stop",mask);
  check(d.PspStopQuiet==expectedPsp,"PSP verdict preserves failed ring/TMR teardown",mask);
  check(restoreCalls==expectedRestore,"GART restore requires every consumer retired",mask);
  check(dummyFrees==expectedFree && poolFrees[3]==expectedFree,"dummy and GART owner survive failed restore or consumer",mask);
  check(viewUnmaps==expectedPsp && poolFrees[2]==expectedPsp,"PSP views and owner survive uncertain teardown",mask);
#ifdef HAS_PHASED_STOP
  check(poolFrees[1]==expectedRestore && (!d.Gfx)==expectedRestore && !d.Psp && (!d.Gart)==expectedFree,
        "owners remain until corresponding retirement succeeds",mask);
#else
  check(poolFrees[1]==1 && !d.Gfx && !d.Psp && !d.Gart,"software owners detach once",mask);
#endif
  check(d.GpuStopUnconfirmed==!expectedFree,"uncertain stop latches device-object quarantine",mask);
  check(!locks&&!regions,"all stop paths balance locks and regions",mask);
  oldUnloads=unloadCalls;oldRestores=restoreCalls;oldFrees=dummyFrees;
  StopPhases(&d);
  check(d.GfxStopQuiet==expectedGfx && d.PspStopQuiet==expectedPsp && d.GpuStopUnconfirmed==!expectedFree,
        "second stop cannot erase failed-stop verdicts",mask);
  check(unloadCalls==oldUnloads && restoreCalls==oldRestores && dummyFrees==oldFrees &&
#ifdef HAS_PHASED_STOP
        poolFrees[1]==expectedRestore &&
#else
        poolFrees[1]==1 &&
#endif
        !locks&&!regions,"second stop cannot repeat destruction or releases",mask);
 }
 for(mask=0;mask<8;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;
  int expected;
  finiQuiet=!((mask>>0)&1);undoFail=(mask>>1)&1;sequenceFail=(mask>>2)&1;lookupFail=0;
  expected=finiQuiet&&!undoFail&&!sequenceFail;memoryRelease=-1;disableFail=0;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;d.IhQuiet=1;d.GfxStopQuiet=1;
  modelAdev.backend=&gfx.Sequence;
  (void)Fini(&d,&gfx,&modelAdev,&undo);
  check(d.GfxStopQuiet==expected && sequenceRelease==expected,"manual Fini preserves precise retirement verdict",mask);
  // A quiet-but-unsuccessful undo can clear StagesDone; Stop must still retain.
  if(finiQuiet) {
#ifdef HAS_PHASED_STOP
   d.PspStopQuiet=1;GfxPrepareStop(&d);GartPrepareStop(&d);
#endif
   GfxStop(&d);
   check(d.GfxStopQuiet==expected && memoryRelease==
#ifdef HAS_PHASED_STOP
         (expected?1:-1) &&
#else
         expected &&
#endif
         d.GpuStopUnconfirmed==!expected,"PnP stop retains earlier manual Fini failure even with cleared stages",mask);
  }
 }
#ifdef HAS_HALT_PHASE
 // Hardware retirement alone must preserve every software owner and page.
 // Exercise actual helpers, including a quiet halt with an unsuccessful undo.
 for(mask=0;mask<8;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;int quiet;
  int oldRelease=sequenceReleaseCalls,oldTeardown=teardownCalls;
  finiQuiet=!((mask>>0)&1);undoFail=(mask>>1)&1;sequenceFail=(mask>>2)&1;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;modelAdev.backend=&gfx.Sequence;
  quiet=HaltEngines(&d,&gfx,&modelAdev,&undo);
  check(d.Gfx==&gfx && gfx.SetUp && gfx.StagesDone==8 &&
        sequenceReleaseCalls==oldRelease && teardownCalls==oldTeardown,
        "halt phase retains owners, stages and all storage",mask);
  check(quiet==finiQuiet && (undo!=0)==undoFail,
        "halt-register result does not erase undo failure",mask);
  ReleaseStoppedStorage(&d,&gfx,&modelAdev,quiet,undo);
  check(sequenceReleaseCalls==oldRelease+1 && teardownCalls==oldTeardown+1 &&
        sequenceRelease==(finiQuiet&&!undoFail&&!sequenceFail),
        "release phase preserves the complete retirement verdict",mask);
 }
#endif
#ifdef HAS_PHASED_STOP
 // Positive ownership paths: full WDDM leaves retired translations disabled;
 // display-only diagnostics retain their explicit firmware-restoration policy.
 for(mask=0;mask<2;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};BC250_PSP psp={0};BC250_GART gart={0};
  lookupFail=undoFail=sequenceFail=unloadFail=restoreFail=disableFail=0;
  mappingFail=reloadQuiesceFail=0;finiQuiet=1;
  locks=regions=unloadCalls=restoreCalls=dummyFrees=viewUnmaps=disableCalls=0;
  memset(poolFrees,0,sizeof(poolFrees));memoryRelease=-1;sequenceRelease=0;
  gfx.StagesDone=8;gfx.SetUp=1;psp.RingUp=psp.TmrUp=1;psp.Pages=&psp;
  gart.Enabled=1;gart.DummyPage=&gart;
  d.Gfx=&gfx;d.Psp=&psp;d.Gart=&gart;d.IhQuiet=1;d.FullWddm=(int)mask;
  StopPhases(&d);
  check(disableCalls==1 && unloadCalls==1,"hardware retirement in both ownership modes",mask);
  check(restoreCalls==!mask,"full WDDM must not restore firmware translation state",mask);
  check(!d.Gfx && !d.Psp && !d.Gart && dummyFrees==1 && memoryRelease==1 &&
        poolFrees[1]==1 && poolFrees[2]==1 && poolFrees[3]==1,
        "successful stop releases all retired owners",mask);
  check(!d.GpuStopUnconfirmed && !locks && !regions,"successful stop remains confirmed and balanced",mask);
  StopPhases(&d);
  check(disableCalls==1 && unloadCalls==1 && restoreCalls==!mask && dummyFrees==1,
        "repeat stop preserves final hardware state without duplicate release",mask);
 }
#endif
 printf("%d checks, %d failures\n",checks,failures);
 return failures?1:0;
}
