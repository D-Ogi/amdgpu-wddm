E16 run 003, 2026-09-21, unit A, Windows boot 2026-09-21T18:28:12 (the same boot as runs 001 and 002).
Files are copied from C:\BC250\e16\out, C:\BC250\e16-umd\out (the install) and C:\BC250\kmdlog as they were written;
budget-reset.txt is the one line of the target's file that belongs to this run. Nothing was redacted: no MAC, serial,
UUID or SSID occurs in these files.

Build: the bc250kmd 0.7.3 binary of run 002 (commit 82d39fb, bc250kmd.sys sha256 b6b942122e21152c...), this time as
the run 2 package of the same clean build: DriverVer 0.7.3.1, UserModeDriverName = bc250umd.dll (three times),
bc250umd.dll (the stub of driver/umd-stub) installed. The ONE difference from run 002 is the package.
Hypotheses H7 and H8 were written into the experiment's README before the run (commit fbae104).

What happened, in order (local time = UTC + 2, file):

21:08  install-x-210808       0.7.3.1 installed over the running 0.7.3.0, every gate closed. Device OK, stage 61:
                              the display-only table is indifferent to a UserModeDriverName in its software key.
21:08  state-pre-gate3        stage 61, presents counting, umd line shows the three names.
21:09  confirm-x-210905       UnconfirmedStarts -> 0 before the gate is touched.
21:09  gate-x-210939          EnableMmio = EnableVram = EnableFullWddm = KeepLog = 1, device disable/enable.
       ring-...-190941-291    the display-only instance stopped by the disable (17 lines).
                              RESULT of the enable: status Error, CM_PROB_FAILED_POST_START again, stages
                              10 20 30 31 32 33 34 35 39 70 79. But dxgkrnl went further than in run 002:
       ring-...-190945-523      16  0.124 wddm: DRIVERCAPS 576 of 576 bytes: wddm 8192 sched 0x45 mm 0x60 paging node 0 flip 0x2 slots 0
                                17  0.124 wddm: QueryAdapterInfo type 1 in 0 out 576 -> 0x00000000
                                18  0.124 wddm: QueryAdapterInfo type 15 in 4 out 20 -> 0xC00000BB
                                19  0.124 wddm: QueryAdapterInfo type 47 in 0 out 4 -> 0xC00000BB
                                20  0.126 stage 70
                              Type 15 is DXGKQAITYPE_PHYSICALADAPTERCAPS, type 47 DXGKQAITYPE_64BITONLYCAPS; the
                              driver refuses both with STATUS_NOT_SUPPORTED. Three QueryAdapterInfo calls, no other
                              DDI of the full table, no object, no TDR. No display event 4101, no bugcheck, no
                              Kernel-PnP error, no live kernel report, dwm.exe still the process of 18:29:13.
21:10  budget-reset           UnconfirmedStarts 1 -> 0 by hand.
21:10  gate-x-211047          gates closed, disable/enable: display-only driver back, stage 61, presents counting.
21:11  state-back             healthy, presents 255. 67.8 C before and after.

Reading: H7 holds. With nothing changed but the UMD name, dxgkrnl asks the second question the offline reading of
its code predicted (type 15), so the stop of runs 001 and 002 was the missing UserModeDriverName. H8 is compatible
with the record and not proven by it: the start still fails, after two refused queries; the record does not say which
check failed. The 0.7.3.1 package stays installed (gates closed) for run 004.
