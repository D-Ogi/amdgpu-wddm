E24 run 007 again, 2026-09-22, bc250kmd 0.7.33 (560ad2b). Fresh boot 13:17:15. Same gates as run 007, flip gate closed, pressure 64 then 256 then 512 MB.

The offset fix of 0.7.33 submitted the first fill: 80 dwords at shadow offset 0x0, 320 bytes, sequence 3, logged as "fence 1 on the SDMA0 ring". The fence slot stayed 0. At 500 ms the path closed itself ("seq 3 in flight, slot 0x0") and the later two submissions never reached the ring. Final tally: 1 hardware submitted, 0 completed, 1 timeout.

IH after the CP fence and before the pressure: client 20 source 181, count 2 (the two gfx fences). IH after the pressure: those same 2, plus client 27 source 0 count 4, plus client 8 source 221 count 1. Source 221 is SDMA0_5_0__SRCID__SDMA_PAGE_FAULT in third_party/linux-amdgpu/irqsrcs_sdma0_5_0.h ("Page Fault Error from UTCL2 when nack=3"). No source 224 (the trap M40's working SDMA fence raises).

No TDR, no bugcheck. The undo's gfx fini printed exit code 3; ih fini, psp unload and gart restore printed 0. The machine was back at stage 61, 68 C.

ring.txt is the driver log ring (the copy on the target was UTF-16; this file is UTF-8). ih-after-fence.txt and ih-after-pressure.txt are the two `ih state` dumps.
