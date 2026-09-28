# M632: KMD159 runtime binding and native control005

Unit A,2026-09-27. Exact source e898ff1ffa310376628f1101f98020acaaa8e008,
SYS5C64097667EB37615CAFD12B291D7281918D0025B5E761C2B74BDCBBBF1644E7,
version0.7.159.1. Transition scripts5eb6e43. In-session transition worker and
installer exit0; CPU DWM4596 and OS boot retained. Collector finishes161
samples without reader timeout. Independent post-check matches loaded version,
SYS and baseline UMD8279AC7F/ICD93B1D1FD. At07:42:23Z healthflags15/guard0,
1000MHz/VID116,66.375C. No new-generation manual confirmation was required.

Twelve non-BC2A opens during startup and one BC2A open during control005
report attempted1/get0/acquire1/token1/bound1/released1. The startup examples
include flags1 and flags0. These complete boolean records remove M627's
truncated membership-witness gap: acquired data matches a live allocation,
is published to the open under lock, and its runtime reference is released.
This proves these measured opens, not every possible teardown interleaving.

Native control005 uses binary0821C9BD (source4633abe), pinned159 runner,
Mode dirty-list. Five copy cases and30 device-accessible residency checks pass,
zero data mismatches, final fence34. Largest verification covers9,371,648 bytes
and2,302,800 copied pixels in2,400 packets. Residency1 does not identify the
physical heap. No TDR is reported. BC2S is exercised, not BGP1 Present.

Control closure07:44:11Z: healthflags15,1000MHz/VID116,66.625C, same boot/DWM,
task removed. Transition cleanup07:44:23Z verifies all three original process
identities terminal and removes its task. The archive helper's final status
query fails because the control task was already removed; archive creation
and transition cleanup had completed. The existing archive was pulled without
rerunning jobs or overwriting evidence.

GPU Present remains0, identity probe1; CPU desktop retained, no active tests.
Next required checks are duplicate/shared opens across devices and owner exit,
then a separately gated GPU Present trial. Full G0 remains open.
JSON/closure receipts are copied unchanged; selected text preserves original
relevant lines with source filenames and UTF-8 decoding. Complete raw archive
and control logs remain private under scratch/g0-hosted. No selected values
were redacted. This run did not request or receive an owner visual verdict.
