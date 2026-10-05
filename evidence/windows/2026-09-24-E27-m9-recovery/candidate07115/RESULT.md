# M365 - Deferred GFX invalidation passes first control; warm PSP interval sets RLCbusy

Exact0.7.115.1 SYS51AD590C759F4F4A82139F24E97E8E151D05EF56F6994FF30291777DA90E91DC, unitA. First control used recoveredboot06:44:52;preflight verifies114closedfull/resetgates,count0,no-full-startmarkers. Install115withoutreboot,resetgate0throughout. No routineACbaseline.

First control passes. Initial CP/MEC/SDMA halt preconditions satisfied. RLCbeforeGART/afterMMflush CNTL0/STATUS2 8;afterPSP CNTL1/STATUS2 8. SuccessfulRLCstage5 commits GFXrequest/dummyread/ACK thenMMflush,retainsCNTL1/STATUS2 8. Startup ready1, allphasescomplete. Unchanged64KiB probe:3residencycycles/4fullrange readbacks,GFX4/4,paging1098/1098,noerrors/TDR. This establishes one hardware positive control for the staged ordering, not general cache/translation coverage.

Stop ring-20260924-050545-889.log:25ownerGTTmappings retired withRLCenabled andbusy clear;explicitRLCstop/PSPunload/GARThardwaredisable/storagecleanup/finalrestore allretainCNTL0/STATUS2 8. One warmenable07:06:07. Pinnedsubnetscan finds0labmatches;SSHstream remainsuncompleted.

Warm ring-20260924-050607-863.log: initialhaltpreconditions satisfied;preGART,hubenables,faultdefaults,MMflush,beforePSP allCNTL0/STATUS2 8. GARTreports284writes (initialGFXrequest deferred). All11PSPcommands rc0/status0. FirstafterPSP snapshot isCNTL0/STATUS2 01000008. IHinit andGFXstages1-4report success. Lastpersistedcheckpoint entersstage5 at0.292s; no completedstage5/translationcommit/CPcheckpoint survives. Do not attribute the failure to a particular stage5 MMIO instruction: intermediate flush callbacks are in-memory logs and may not have persisted.

This measures a clear-to-busy interval across PSPinitialization with noinitialhostGFXinvalidation. M362's request-associated transition remains valid for114, but avoiding that request is insufficient for warmreentry. PSP command success doesnotestablish healthy reloadedRLC. No per-commandRLCsnapshot exists in115; specific firmwarecommand attribution is unproven.

Onefailure-recoveryAC OFF8s/ON,Windowsboot07:07:39. RetainednativeSSH PID66756terminated onlyafterverifiedAC;localexit4294967295is transportcleanup,notremotescriptcompletion.17newpersistent snapshotscollected beforeclosed-gatePnPrecovery. IndependentfinalCLIinfo succeeds:115stage61/36presents,confirm0,executiongates0/reset0,count0,SSHhealthy. NoextraOS/DWMrestart/manualaction. Hostlogs redactPCIidentity andhardware/interface lines;rawsnapshots/probe unchanged.80.2Wfailuretelemetry auxiliaryonly,unknownsampleage/calibration.

Next isolate PSPfirmware-load RLCstate transitions and examine the supported reload/reset/stop dependencies in localAMDreference. Preserve successfulstagedfirstcontrol and translationbarrier; do not skip firmwareloads or copy stale register values without a lifecycle contract. No unchanged115warmretry. WarmM9acceptance and broaderDMA/cache/alias/lifetime/performance remainopen.
