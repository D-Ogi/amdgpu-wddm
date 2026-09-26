# M367 - Warm RLCbusy first observed after SDMA0 firmware load

UnitA exact0.7.116.1 SYSBA276AA2AD486FB616A867CE4700AB98533411219DB4954310F2B0289A4C6926. Recoveredboot07:07:39 independentlyverified115closedgates/reset0/count0/no fullstartupmarkers.116installedwithoutreboot. Resetgate0throughout;no routineACbaseline.

Firstcontrol enable07:15:49 passes. Before/afterPSPringcreate andcommands1-10 allRLC CNTL0/STATUS2 8. Command11(RLC_G,type8) reportsrc0/status0,firstsnapshotCNTL1/STATUS2 01004008;afterPSP1/01000008. ByafterRLCstage5translationcommit,1/8. Startupready1 andunchanged64KiBprobePASS:3residencycycles/4fullreadbacks,GFX4/4,paging1093/1093,noerrors/TDR. This is a positivecontrol that RLCbusy immediatelyafterfirmwareload can occur duringworkinginitialization; it is not independently a failure/quiet predicate.

One closed-gatestop followedbywarmenable07:17:01 losesSSH. Pinnedsubnetscan0matches;originalstreamretaineduntilverifiedrecoveryAC. Warm ring-20260924-051701-893.log:
- preGART,afterhubconfiguration/MMflush,beforePSP: CNTL0/STATUS2 8;
- before/afterringcreate andcommand1 SETUP_TMR:0/8;
- command2 LOAD_IP_FW,type9,SDMA0,33536bytes,rc0/status0: firstpostcommandsnapshot0/01000008;
- commands3-11allrc0/status0,postcommandsnapshotsremain0/01000008;
- RLCstage5reacheshostGFXrequest/read/ACK andtranslationcommit: CNTL1/STATUS2 01004008 atcommit;
- entersstage6/CP1;lastpersistedcheckpoint scheduler-read0.319s.

The firstchangedinterval is command2(SDMA0) plusobservationlatency;notproof of an internalPSPinstruction orSDMAfirmwarefault. Unlike115,116persistscompletedstage5/translationcommit thenCP1. Difference mayincludetimingfromaddedreads; no distinctcausalhanginstructionclaim. Command11success alone doesnotprove RLChealthy. ExistinginitialGFXinvalidationdeferral isinsufficient.

Recovery:oneOFF8s/ONverified,boot07:18:37. RetainedSSH PID57152targetedcleanupafterverifiedAC,localexit4294967295notremotescriptcompletion.20newpersistentsnapshotscollectedbeforeclosed-gatePnPrecovery. FinalindependentCLIinfo116stage61/39presents,confirm0,all executiongates0/reset0,count0,SSHhealthy. NoextraOS/DWMrestart/manualaction. Hostlogs redactPCIidentity/hardwareinterface lines;rawsnapshots/probeunchanged.77.1Wauxiliaryfailuretelemetry unknownage/calibration.

Next review SDMA0firmware-start dependencies on retainedRLC/SDMAstate. Currentbc250_sdma_hw_fini source already clears RB_ENABLE/IB_ENABLE andcontextswitch beforeF32HALT, so do not assume it merelyhalted anenabledring. No loaded-firmware skip,newresetmask orreorder justified solelybythis interval. Preservecoldpositivecontrol,comparefirmware-induced engine/RLCstate against AMDreference and exactpriorretirement. WarmM9acceptance andbroaderDMA/cache/alias/lifetime/performance remainopen.
