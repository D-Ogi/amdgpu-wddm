
int main(void)
{
 unsigned mask;
 for(mask=0;mask<128;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};BC250_PSP psp={0};BC250_GART gart={0};
  int expectedGfx,expectedPsp,expectedRestore,expectedFree,oldUnloads,oldRestores,oldFrees;
  lookupFail=(mask>>0)&1;finiQuiet=!((mask>>1)&1);undoFail=(mask>>2)&1;
  sequenceFail=(mask>>3)&1;unloadFail=(mask>>4)&1;restoreFail=(mask>>5)&1;
  d.IhQuiet=!((mask>>6)&1);
  expectedGfx=!lookupFail&&finiQuiet&&!undoFail&&!sequenceFail;
  expectedPsp=expectedGfx&&!unloadFail;
  expectedRestore=expectedGfx&&expectedPsp&&d.IhQuiet;
  expectedFree=expectedRestore&&!restoreFail;
  locks=regions=unloadCalls=restoreCalls=dummyFrees=viewUnmaps=0;
  memset(poolFrees,0,sizeof(poolFrees));memoryRelease=-1;sequenceRelease=0;
  gfx.StagesDone=8;gfx.SetUp=1;psp.RingUp=psp.TmrUp=1;psp.Pages=&psp;
  gart.Enabled=1;gart.DummyPage=&gart;
  d.Gfx=&gfx;d.Psp=&psp;d.Gart=&gart;d.GfxStopQuiet=d.PspStopQuiet=1;
  GfxStop(&d);PspStop(&d);GartStop(&d);
  check(d.GfxStopQuiet==expectedGfx,"GFX result includes halt, undo and sequence fault",mask);
  check(sequenceRelease==expectedGfx,"Fini releases sequence pages only after clean halt and undo",mask);
  check(memoryRelease==(expectedGfx&&d.IhQuiet),"GTT release needs engine and IH retirement",mask);
  check(unloadCalls==expectedGfx,"PSP unload never follows unconfirmed GFX stop",mask);
  check(d.PspStopQuiet==expectedPsp,"PSP verdict preserves failed ring/TMR teardown",mask);
  check(restoreCalls==expectedRestore,"GART restore requires every consumer retired",mask);
  check(dummyFrees==expectedFree && poolFrees[3]==expectedFree,"dummy and GART owner survive failed restore or consumer",mask);
  check(viewUnmaps==expectedPsp && poolFrees[2]==expectedPsp,"PSP views and owner survive uncertain teardown",mask);
  check(poolFrees[1]==1 && !d.Gfx && !d.Psp && !d.Gart,"software owners detach once",mask);
  check(d.GpuStopUnconfirmed==!expectedFree,"uncertain stop latches device-object quarantine",mask);
  check(!locks&&!regions,"all stop paths balance locks and regions",mask);
  oldUnloads=unloadCalls;oldRestores=restoreCalls;oldFrees=dummyFrees;
  GfxStop(&d);PspStop(&d);GartStop(&d);
  check(d.GfxStopQuiet==expectedGfx && d.PspStopQuiet==expectedPsp && d.GpuStopUnconfirmed==!expectedFree,
        "second stop cannot erase failed-stop verdicts",mask);
  check(unloadCalls==oldUnloads && restoreCalls==oldRestores && dummyFrees==oldFrees &&
        poolFrees[1]==1 && !locks&&!regions,"second stop cannot repeat destruction or releases",mask);
 }
 for(mask=0;mask<8;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;
  int expected;
  finiQuiet=!((mask>>0)&1);undoFail=(mask>>1)&1;sequenceFail=(mask>>2)&1;lookupFail=0;
  expected=finiQuiet&&!undoFail&&!sequenceFail;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;d.IhQuiet=1;d.GfxStopQuiet=1;
  modelAdev.backend=&gfx.Sequence;
  (void)Fini(&d,&gfx,&modelAdev,&undo);
  check(d.GfxStopQuiet==expected && sequenceRelease==expected,"manual Fini preserves precise retirement verdict",mask);
  // A quiet-but-unsuccessful undo can clear StagesDone; Stop must still retain.
  if(finiQuiet) {
   GfxStop(&d);
   check(d.GfxStopQuiet==expected && memoryRelease==expected &&
         d.GpuStopUnconfirmed==!expected,"PnP stop retains earlier manual Fini failure even with cleared stages",mask);
  }
 }
 printf("%d checks, %d failures\n",checks,failures);
 return failures?1:0;
}
