# M677: KMT WSI Present is still refused with interop1 and GPU DWM

Unit A, runner c2355e3, exact KMD164/933f383 (SYS9B9B99D3),
routerA19608D4, UMD5C74BF98 and hosted ICD3508416F. GPU DWM7564 started
14:12:13.7355094Z. The normal monitor confirmed the new adapter generation;
the launch gate observed FULL|READY|VISIBLE|CONFIRMED at ready age64575ms,
fresh completion age2ms, generation115997992792/epoch5. The worker's health,
clock and exact artifact preflight passed. No health requirement was weakened.

## Actual route result

Probe6084, EXE228EBEC0, directly loaded private ICD0CD4A98D as wsi-kmt.dll,
negotiated ICD7, selected BC250 and created its640x480 square-corner window.
CPU fallback was absent. The first red frame's vkQueuePresentKHR returned
VK_ERROR_SURFACE_LOST_KHR (-1000000000). The probe stopped on that first error
(exit2), with no swapchain recreation or automatic CPU fallback. No image capture
or content pass resulted. Its CSV names path kmt and copy_us0; that is a failed
present, not successful no-copy presentation.

The ICD logged a non-UMD present context0x40000980, an exclusive VidPn ownership
query returning0, and D3DKMTPresent returning0xC01E0342 for HWND0xE049C,
source allocation0x40000400. The sole probe ETW event184 at14:13:26.6421569Z
has flags193 (Blt/SrcRectValid/DstRectValid), source0x40000400, destination0,
VidPn source0, and ReturnStatus3223192386 (STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE).
There are no probe Blit_Info, BlitCancel or PresentHistory events.

The decoder found10 Blit_Info events from other processes and1440 successful
DWM7564 Present events as positive controls. xperf reports zero lost events and
zero lost buffers. KMD GPU Present submits remain8/rejected0/failed0 before the
probe and in retained startup-log-075 after the refusal, before adapter rollback.
Thus this candidate is refused at Windows admission before a Blt reaches the
KMD; it does not test BGP1 copy content or demonstrate a KMD copy failure.
The earlier interop0 refusal E46/M601 reproduces here with interop1 and actual
GPU DWM. These conditions alone are insufficient for this ICD's bare windowed
Blt route. This does not prove every redirected or shared-buffer route impossible.

## Closure and scope

Main recorded61.997s before failure handling (success=false). Its early-abort
policy ended the trial before180 seconds. CPU DWM8220 restored14:13:41.7847791Z.
Collector11524 ended14:15:10.3972215Z,158 samples,no timeout. All four tasks were
removed after collecting archive9,518,669bytes and ETW78,643,200bytes.
Fresh14:16:49Z confirms same boot,CPU8220,exact164,UMD8279AC7F/registeredICDCF3948D6,
health15,guard0,gates0,1000MHz/VID116,66.875C and no test processes. No promotion.

The relevant M674 CPU WSI control remains the content positive control. This
failure concerns the standalone WSI window route, also required by M12. G0 still
requires the actual GPU compositor's correct output, DWM-correlated completed
work and exclusion of steady-state CPU full-frame copies; neither this rejection
nor isolated packet control M673 substitutes for those requirements.

Private full trace, decoded events and screenshots remain under
scratch/g0-hosted/dwm035. Published excerpts select only the winsys Present lines;
the raw stderr and trace are retained privately. Decoder source and zero-loss
statistics are included for reproducibility.
