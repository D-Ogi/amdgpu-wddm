# GPU desktop G0 implementation status

**Implemented and measured on unit A**, DWM050/M723, audited by M724. The native D3D runtime drives hosted Zink/RADV allocations and GPU work; actual DWM4944 composition has4498 completed DMA pairs and4274 exact wait/Present/signal joins. Both independent primary readback and composed-image controls pass40712 pixels. The combined native GPU window completes2824 frames and exits0.

The bounded no-copy acceptance uses the agreed four-item list: application-window uploads classified, known positive KMD software-blit control, descriptor writes distinguished from image writes, and a final persistent-map census. DWM050 has82zero-blit samples and24635reconciled maps with no presented runtime-surface map, no crossing image mapping and no unfinished writer span. M702 supplies the positive control; its WddmPresentBlit implementation matches170 exactly. This is an instrumented claim for the tested presentation path, not all possible memory writes in Windows.

[Measured run and limitations](../../evidence/windows/2026-09-28-E34-dwm050-gpu-desktop/RESULT.md). [Completion audit](../../evidence/windows/2026-09-28-E34-g0-completion-audit/RESULT.md).

The trial lasts124.910s and restores CPU170, health15, without reboot. Owner's180-second limit remains. Full M13 lifecycle/30-minute stability/fault/soak gates and permanent GPU promotion are separate; G0 completion does not silently waive them. Startup VSync gaps are recorded separately from the steady render interval, which has no missing VSync reports.
