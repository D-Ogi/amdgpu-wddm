# M368 - SDMA reload dependency review

PROVENANCE: Linux amdgpu v6.18 commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449, AMD MIT. Source hashes attached. This is source evidence only; no new lab run or register write.

Question fromM367: why does the first post-SDMA0 firmware-load snapshot show RLCbusy only onwarmstartup? Existingdata boundstheinterval,notPSPinternalbehavior.

1. Current bc250_sdma_hw_fini disables contextswitch,clears both GFX_RB_CNTL.RB_ENABLE andGFX_IB_CNTL.IB_ENABLE on eachrealinstance,setsF32_CNTL.HALT,andcompares64-bitRPTR/WPTR. It doesnot requestFREEZE orclearCNTL.UTC_L1_ENABLE. Currentbring-up setsUTC_L1_ENABLE=1. Thus ordinaryteardown doesnot explicitly restore that field; actualpre-reloadhardwarevalue stillneedsmeasurement. Do not claimenabledqueues fromthe existinghalt-onlysummary.
2. AMDsdma_v5_0_stop_queue is a differentpath usedforengine-reset: entersRLCsafe mode,disablesselectedring,requestsSDMA_FREEZE.FREEZE andpollsFROZEN; onpolltimeout it acceptsSTATUS1low10bits allset asidle. ThenhaltsF32 andclearsUTC_L1_ENABLE,exitssafemode. sdma_v5_0_restore_queue enterssafemode,clearsFREEZE,resumesringwithsavedpointer,exitssafemode. Thesearecoupled stop/restart steps,not a recipe to copyonebitwrite inisolation.
3. amdgpu_sdma_reset_engine stopskernelqueues,invokesamdgpu_sdma_soft_reset,thenstartskernelqueues. sdma_v5_0_soft_reset_engine constructsSOFT_RESET_SDMA0 shiftedbyinstance,read/OR/write/read,50us,clear/write/read. Generic IPsdma_v5_0_soft_reset isonlyaTODOreturn0. The per-enginecallback istherefore a real registersequence,whereas thegenericreturnaloneisnot resetproof. Neither proves successfulfirmwarereload onunitA.
4. amdgpu_gfx_rlc_enter_safe_mode firstrequiresRLCenabled,thenapplicablecg_flags,andtracksin_safe_mode. gfx_v10_0_set_safe_mode writesAMD'sCMD+MESSAGE request andpollsCMDclear,voidreturn. A Windowsport must distinguish requestcompletion and preserve the pairedexit; a timeout cannot silentlyestablish a safe state. RLCdisabledbeforePSP cannot be made to servicea handshake by merelycallingthishelper.
5. Source discrepancy: v6.18sdma_v5_0_enable computesinst_mask=GENMASK(num_instances-1,0),then passes1<<inst_mask tosdma_v5_0_gfx_stop. Fornum_instances2 thisselectsbit3,notbits0and1. Ourshimexplicitlyloopsrealinstances0and1. Keepthatboundedloop;do notcopytheshift asan unquestionedreference. Updatedshimcomment/provenance tostate thisdeviation; noexecutablechange. This sourcefindingdoesnotprove whattheunverified6.18.52runtimeexecuted or explainits failure.

Next concrete implementation/review boundary:
- Evaluate a complete SDMA queue-quiescence phase while currentRLC/firmware/backing remainowned: safe-mode request/ack whereapplicable,ringdisable,freeze/idlewitness,halt,UTC_L1disable,pairedsafe-modeexit. Preservebacking andpendingtranslations onfailure.
- Pair itwithanexplicitunfreeze/newring-preparation contract beforeSDMA admission. Decide and document whetherfreeze persists acrossPSPreload; do not inferfirmware behavior from host register names.
- Beforehardware,deriveallregisteraddresses/masksfromoriginalAMDheaders andconfirmMMIOallowlists;actual-sourcehostmodel mustcover successfuldrain/order anddiscriminateomittedfreeze/cachephase. Preserveordinarycoldcontrol and comparewarmRLC/SDMAstates aroundPSPcommand2.
- Per-engine reset is a separatecandidate withtheseprerequisites,not an extra mask toadd blindly. ExistingGFXinvalidationdeferral andreadinessbarrier remainrequired bycurrenttested design untilchanged withcontrols.

Noreset/firmware-reorder/cache-disable has been applied in thisreview. Installed116display-onlyboot07:18:37 fromM367 remainslastverifiedstate. WarmM9/cache/DMA/alias/lifetime/performance acceptance remainopen.
