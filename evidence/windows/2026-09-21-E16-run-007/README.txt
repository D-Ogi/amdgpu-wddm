E16 runs 006 and 007, 2026-09-21
================================

Run 006: bc250kmd 0.7.6 (commit 81428ed) = 0.7.5 plus tracing wrappers on the child and VidPN DDIs.
  22:10:23  install (gates closed), stage 61, confirmed.   22:11:36  gate -Full 1: status OK, stage 39, screen black.
  22:12:17  ring-open6: QueryChildRelations ok, QueryDeviceDescriptor 0xC01D0001 (no EDID, as designed),
            RecommendMonitorModes ok, IsSupportedVidPn ok, and EVERY EnumVidPnCofuncModality -> 0xC01E030A
            (STATUS_GRAPHICS_INVALID_FREQUENCY). The display side is refused, not skipped.

Run 007: bc250kmd 0.7.7 (commit d19081d): the full table's modes carry 60 Hz instead of FREQUENCY_NOTSPECIFIED.
  22:16:29  install (gates closed), stage 61, confirmed.   22:17:43  gate -Full 1 (gate-x-221743 is cut short).
  ~22:18    BUGCHECK 0x113 VIDEO_DXGKRNL_FATAL_ERROR, seen by the owner on the screen. Restart with the gate still
            open in the registry: second bugcheck (dump written 22:22:26). Third boot: the start budget refuses the
            start (stage 90, UnconfirmedStarts 2); the owner brought the machine up in safe mode with networking
            (boot 22:24:45). Gate closed by hand over SSH, budget cleared.
  bugcheck-analyze-excerpt.txt: 0x113, Arg1 0x1C, Arg2 1, Arg3 1 (= the RotationSupport value we passed, Identity
            only), in dxgkrnl!DMMVIDPNPRESENTPATH::SetRotationSupport, under
            DXGK_VIDPNTOPOLOGY_INTERFACE::UpdatePathSupportInfo <- bc250kmd!Bc250EnumVidPnCofuncModality
            <- VIDPN_MGR::FormalizeVidPnChange <- win32kbase!DrvSetDisplayConfig, process csrss.exe.
  Read in the lab's dxgkrnl.sys 10.0.22621.6199 (cdb -z, uf SetRotationSupport): if the driver supports path
            independent rotation and g_OSTestSigningEnabled is set, the primary clone path's support must have bit
            0x10 (Offset0) [else subtype argument 1] and none of 0xE0 [else 2]; another path needs any of 0xF0
            [else 0]. When the driver does not support path independent rotation dxgkrnl ORs 0x10 in by itself.
            SetScalingSupport has no such exit.
  ring-open7-222755.log is the safe-mode instance (refused start), not the crashed one: the ring of a bugchecked
            instance lives in the dump only.
