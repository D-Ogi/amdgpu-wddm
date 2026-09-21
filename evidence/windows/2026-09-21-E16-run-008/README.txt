E16 run 008, 2026-09-21: bc250kmd 0.7.8 (commit aaf8050), UMD stub package, frozen by tools/packagecheck (PASS)
bc250kmd.sys sha256 prefix af0497b95c8ee6b9. Changes against 0.7.7: RotationSupport.Offset0, one-shot gate.

  22:38     install (gates closed), stage 61, confirmed; gate -Full 1.
  ~22:40    BUGCHECK 0x3B SYSTEM_SERVICE_EXCEPTION (c0000005) in dxgmms2!VIDMM_DMA_POOL::AddDmaBufferToPool, process
            csrss.exe, under win32kbase!DrvSetDisplayConfig -> dxgkrnl!CCD_TOPOLOGY::ApplyTopologyOnAdapter ->
            SESSION_ADAPTER::CreateCddDevice -> DXGDEVICE::CreateContext -> DXGCONTEXT::EnsurePriviledgedDmaPool.
            The machine did not restart by itself (AutoReboot was 0); the owner restarted it.
  22:45:00  boot. The one-shot gate worked: EnableFullWddm was 0 on disk, the display-only driver started, stage 61,
            picture back, no safe mode. AutoReboot set to 1 at the owner's request.

bugcheck-analyze-excerpt.txt has the stack and the driver's log ring from the dump: H12 holds (five
EnumVidPnCofuncModality calls, all successful), then CreateProcess / CreateDevice (flags 0x2) and CreateContext with
flags 0x6 = GdiContext | VirtualAddressing, answered with DmaBufferSize 4096 and DmaBufferSegmentSet 0.

Also in this evidence: the shutdown bugcheck between runs 007 and 008 (22:36, leaving safe mode) was 0xCE
DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS in rtwlanu.sys (the USB Wi-Fi dongle's driver, ETW callback
after unload) - not ours.
