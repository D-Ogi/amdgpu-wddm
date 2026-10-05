E24 run 008, 2026-09-22, bc250kmd 0.7.34 (02101b1). Fresh boot 13:43:07. Same gates as run 007, flip gate closed. Pressure 64, 256, 512 MB.

The four fills are the same 872,415,232 bytes as run 005. The packet address is now the MC one: physical 0x271C62000 became MC 0xF401C62000, and 0x281C62000 became 0xF411C62000. Three submissions completed on SDMA0 (fences 1, 2 and 3 reported by the paging DPC path). The IH dump after the pressure shows client 8 source 224 count 3, the trap, and client 20 source 181 count 2, the gfx control fences. No source 221, no client 27. Node 1 ended at 3 hardware submitted, 3 completed, 0 timeouts, 0 refused, 0 buffer switches. No TDR, no bugcheck, gfx fini exit 0, 67-73 C.

ring.txt is the driver log ring, UTF-8. run-008-console.txt is the script transcript, including the IH vector lines the earlier runs grepped away.
