# M360 -113retirement keeps RLCbusy clear; next startup becomes busy again

Exact0.7.113.1 SYS17AED863C702EDA8367616663F6953455B9B67E3479E3453A3F2EB01EF47C1D7.
First-control boot06:23:38 after normal shutdown/one8sACbaseline. Exact installed
hash/version verified; reset gate0 throughout. Fullstart/unchanged64KiBcontrol
pass:3residencycycles/fourfullwordreadbacks,GFX4/4,paging1990/1990,noerrors/TDR.

Stop ring-20260924-042643-536.log: afterCPstopCNTL1/STATUS2 8. Request00F80001,
dummyread00F80001,ACK1 andsuccessfulbothhubflush retainCNTL1/STATUS2 8.25ownerGTT
mappings retired withbackingretained. RLCstop changesCNTLto0 withSTATUS2still8.
PSP TMR/ring unload,GARThardwaredisable,storagecleanup andfinalrestore retain
CNTL0/STATUS2 8,allreadstatus0. The former stop-time busy transition is absent
in this one instrumented trial. No post-RLCstop mappingflush is observed.

One same-boot fullstart at06:27:20 losesSSH. ConfiguredTCPendpoints down and
full pinned-subnet discovery findsnolab. No unchangedretry. Originalstream
session85868/sshPID46196 retained until verifiedrecoveryAC,then targetedlocal
transportkill;exit4294967295is localcleanup,notremotescript completion.

Persisted warm ring-20260924-042720-869.log: GARTinitialization reports success,
beforePSP CNTL0/STATUS2 01000008. All11PSPcommandsrc0/status0;afterPSP same
CNTL0/busy. Lastpersistedcheckpoint CP1scheduler-read at0.320s. No exactfaulting
MMIOinstruction claim. Between completed stop andbeforePSP onnextstart, busy
reappears; startupGARTinitialization includes invalidation and is the next
hypothesis to isolate. There is no immediate pre-GART RLCsnapshot in this trial,
so do not claim a specific startuprequest was directly observed causing it.

Onefailure-recoveryAC restoresboot06:29:13. Persistentlogs collected before
closed-gate PnPrecovery. Finalindependent113info stage61/48presents,fullgate0,
resetgate0,CLIconfirm0,UnconfirmedStarts0,SSHhealthy. Total2ACcycles: onebaseline,
onefailure recovery. NoadditionalOS/DWMrestart ormanualowneraction.
Power samples preserved as auxiliarytelemetry only;unknownage/modelcalibration.
Hostlogs redacthardwareidentity;rawKMD/probe artifactsunchanged.

Next trace startup GARTrequest boundaries and inspect whether PSP's firmware/
ring memory requires GFXHUBtranslation at that moment. Do not blindly remove
initial invalidation or reorderPSP/GART: freshstart/firmwarevisibility must stay
correct. Any deferred GFXHUBinitialization needs explicit dependency proof and
bothcold/warm controls. Warmreentry remains unresolved despite the narrower
retirement improvement. BroaderM9DMA/cache/alias/resource/performance remainopen.
