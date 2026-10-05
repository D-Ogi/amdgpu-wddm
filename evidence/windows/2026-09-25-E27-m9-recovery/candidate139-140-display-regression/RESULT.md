# M451 -139/140 raster and timing controls, physical display regression

Unit A, 2026-09-24/25. M9 acceptance remains incomplete.

139 SYS6A3B68F491423BBECA5A11EB507131BED4E10DD649C9537CDAD2AB9415E21F25
started full viaPnP with native1000MHz/VID116, retaining Windows/DWM.
-1024 real GetScanLine calls:986 active,38 blank,984 activechanges, range0..1199,
 no API failure; previous136 control was1024 blank/line0.
-16 complete named raw observations: stable paired timing tuples, matching
 primary/earliest-in-use, pendingclear. This is stable-scanout evidence only.
-Actual decoder and QueryDisplayConfig agree: active1920x1200,total2080x1235,
 pixel154MHz, refresh59.950171286Hz. Independent1076frame increments over
17.9467857s give59.955026Hz, discrepancy0.087frames, inside counter quantization.
-Runtime firmware query1472B returns cached ME/PFP/CE/MEC/MEC2/RLC/SDMA and
 actual SMU0x00580600 (versions inrawsamples).64MiB eviction/readback control
 passes; D3Dsharedbidirectional/redblue and307200greenpixels pass.

These passes did NOT establish visible output: physicalmonitorblack confirmed
byowner. Raw BLANK_CONTROL=0x101 inall16observations. DWM generated heavy paging
whilehardwareflipcountstayed1. OneDWMrestart23:49:39 producedaGDI desktopimage,
butownerconfirmedphysicaldisplaystillblack. Do not useGDI/FBDUMPpixelcontents
alone as proofthat theoutputpipeline isunblanked. NoAI/Vulkanworkloadfollowed.

A136rollbackwaslaunchedbeforeownersaidfixcurrent/norollback. Forcedinstallation
autoenabled136; scriptstoppedatitsunexpectedenablecheck.136Code43followed.
Currentdirectionisrepair139-derivedcode, notrollback.

140 SYSBBDB196F726226CC1B435F94EDCDD02E458F28F6507544B26EFF0BE5CE4B9252
fixestheprovedsoftware-onlyshow shortcut: hardwareblank with DcnBlankedFALSE
isnowreadandclearedwhenashowrequestarrives. Old139fails3newhostchecks;
140passes141visibility/588restorechecks. Thisdidnotbyitselfresolvephysicalblack.
Initial140startCode43wasaprocedureerror: INFresetEnableMmio0whileNativeSmu1
remained. Stagehistoryreachedframebuffermappedthenfailed. Afterapplyingthefull
profileAFTERinstallation,140startedfull00:03:32withoutOSrestart.
140raster1024calls:1002active/22blank,0failure;16rawsamplesstillblank0x101.
ACdisplaytimeoutis0. Bothsession0displayrequiredandinteractivewake/mousemove
leftblank0x101. Furthervisibilitydeliverydiagnosisrequired; noforcedunblank
onordinaryPresentisjustifiedbecauseMSrequiresPresentwhilemonitorsareoff.

Latestdeployed140isadiagnosticcandidatewithphysicaldisplayfailure,notahealthy
baseline. Sourcevisibilityfirst32logsareinsufficientundertheringwrap, motivating
retainedper-devicediagnosticsnext. Theownerreportedblack/backlight/noinputimage.
Rawsource/snapshots/logsretained; instanceidentifierlinesredacted.Screenshots
remainprivate. FullM9coldstart/resume/cache/lifetime/preemption/performanceopen.
