E16 run 009, 2026-09-21: bc250kmd 0.7.9 (commit 1b6be3c) as the UMD stub package 0.7.9.1
=========================================================================================

Package frozen before the run: package-manifest.txt (tools/packagecheck --model full --load, PASS, 25 checks).
bc250kmd.sys sha256 prefix 35971a781dcf4e9a. Change under test: an aperture segment (2), named in
DXGK_CONTEXTINFO.DmaBufferSegmentSet (H13, facts M70).

Timeline (local time, unit A, one boot: 22:45:00 before and after)
  23:07:15  install, every gate closed: display-only start, stage 61 (state-pre-gate9), confirmed.
  23:08:02  gate -Full 1 with EnableMmio and EnableVram (gate-x-230802). Full table starts: status OK, CM_PROB_NONE.
  23:08:34  ring-open9: system context (flags 0x5), VidPN negotiation (all calls succeed), CreateProcess/CreateDevice
            flags 0x2, CreateContext flags 0x6 answered "dma 4096 bytes segment set 0x2" - and no bugcheck.
            Then GetStandardAllocationDriverData type 1 and type 2 (1920x1200, format 21), CreateAllocation and
            OpenAllocation twice, BuildPagingBuffer operations 12 (FlushTlb) and 9 (VirtualFill), CommitVidPn
            (stage 50), SetVidPnSourceAddress segment 1 address 0xF400ACE000, software VSync on, ControlInterrupt
            type 3 on/off, and from 3.4 s Present + SetRootPageTable + SubmitCommandVirtual, fence 1 onwards.
            No MapApertureSegment call was logged (operation 5 never appears in the summary).
  23:09:16  ring-summary5-230916: 73 presents, 73 virtual submissions, last completed fence 73, 0 preemptions, no TDR,
            4277 VSync ticks of which 434 reported, 9 objects alive. Screenshot through the overlay: black.
  23:10:27  d3d-x: D3D11 hardware device 0x887A0004 (DXGI_ERROR_UNSUPPORTED), WARP S_OK feature level 0xB000.
  23:10:46  ring-summary5-231046: 160 presents, 160 submissions, last completed fence 160, no TDR.
  23:11:03  gate closed again (the gate was one-shot: a plain reload). Display-only table, stage 61, presents
            counting (33, then 57), picture back (overlay screenshot 817 kB against 3.6 kB for the black ones).

Event counters (state-*.txt): Display 4101 (TDR) 0; live kernel reports 0; BugCheck 1001 stays 1 (run 008's).
"dwm / Dwminit" events grow while the full table is up: 7 -> 45 -> 125. Not looked into in this run.
Temperatures 70 to 75 C.
