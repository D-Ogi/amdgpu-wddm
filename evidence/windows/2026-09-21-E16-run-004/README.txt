E16 run 004, 2026-09-21, unit A, Windows boot 2026-09-21T18:28:12 (the same boot as runs 001 to 003).
Files are copied from C:\BC250\e16\out, C:\BC250\e16-umd\out (the install) and C:\BC250\kmdlog as they were written;
budget-reset.txt is the one line of the target's file that belongs to this run. Nothing was redacted: no MAC, serial,
UUID or SSID occurs in these files.

Build: bc250kmd 0.7.4 (commit fc3f76f, clean worktree, bc250kmd.sys sha256 c4ad531286bbf43c...), UMD stub package
0.7.4.1 (UserModeDriverName = bc250umd.dll). New against run 003: DxgkDdiSetStablePowerState, SupportDirectFlip,
FlipCaps.FlipIndependent, segment Flags.DirectFlip, SupportPerEngineTDR with its three DDIs, CollectDbgInfo,
SupportSmoothRotation. Run 004 was announced in the experiment's README before it happened (commit fbae104).

What happened, in order (local time = UTC + 2, file):

21:20  install-x-212040       0.7.4.1 installed over the running 0.7.3.1, every gate closed. Device OK, stage 61.
21:21  state-pre-gate4        stage 61, presents counting.
21:21  confirm-x-212138       UnconfirmedStarts -> 0 before the gate is touched.
21:22  gate-x-212211          EnableMmio = EnableVram = EnableFullWddm = KeepLog = 1, device disable/enable.
       ring-...-192213-472    the display-only instance stopped by the disable (17 lines).
                              RESULT of the enable: status Error, CM_PROB_FAILED_POST_START, stages
                              10 20 30 31 32 33 34 35 39 70 79 - and between 39 and 70 a different story:
       ring-...-192217-705    67 lines. In order, all within 6 ms of StartDevice returning:
                                QueryAdapterInfo type 1  DRIVERCAPS (flip 0x12, tdr 1 dflip 1 rot 1)   success
                                QueryAdapterInfo type 15 PHYSICALADAPTERCAPS                           refused 0xC00000BB
                                QueryAdapterInfo type 47 64BITONLYCAPS                                 refused 0xC00000BB
                                QueryAdapterInfo type 13 GPUMMUCAPS                                    success
                                QueryAdapterInfo type 14 PAGETABLELEVELDESC, four times                success
                                GetNodeMetadata node 0
                                QueryAdapterInfo type 11 QUERYSEGMENT4 pass 1 and pass 2: 1 segment, flags 0x00080404,
                                  gpu 0xF4008CA000, cpu 0x2708CA000, size 0x1FD736000, paging buffer segment 1, 65536 bytes
                                QueryAdapterInfo type 10 HISTORYBUFFERPRECISION (in the summary; its line is not among
                                  the logged refusals because it succeeded)
                                QueryAdapterInfo type 16 DISPLAY_DRIVERCAPS_EXTENSION                  refused 0xC00000BB
                                CreateProcess flags 0x1, CreateDevice flags 0x1 twice,
                                CreateContext node 0 engine 0x1 flags 0x5 (SystemContext | VirtualAddressing),
                                  answered: dma 4096 bytes, segment set 0, lists 0/0, caps 0x1
                                DestroyContext, DestroyDevice twice, DestroyProcess
                                stage 70 (StopDevice)
                              Never entered: GetRootPageTableSize, SetRootPageTable, CreateAllocation,
                              GetStandardAllocationDriverData, BuildPagingBuffer, any submission, any VidPN DDI,
                              ControlInterrupt, SetStablePowerState, the per-engine TDR DDIs, CollectDbgInfo (0 calls).
                              Objects 2/2 devices, 1/1 context, 1/1 process, 0 alive. No TDR.
                              No display event 4101, no bugcheck, no Kernel-PnP error, no live kernel report.
21:23  budget-reset           UnconfirmedStarts 1 -> 0 by hand.
21:23  gate-x-212318          gates closed, disable/enable: display-only driver back, stage 61, presents counting.
21:24  state-back             healthy, presents 80. 67.4 C before and after.

Reading: the refusals the offline reading predicted for the 0.7.3 table are gone; dxgkrnl accepts the caps, the GPU
MMU description, the node and the segment, creates its system process, devices and context, and then takes them down
again in order and stops the device. The record does not say why. Whether type 16's refusal matters is not known
(types 15 and 47 were refused in run 003 as well and the start went on past them here).
