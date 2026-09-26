E23 run 001, 2026-09-22 06:14: bc250kmd 0.7.22 (4c233e3, .sys sha256 prefix 9d137aa9dc44fec8) on unit A after a
fresh warm restart (boot 06:11:58, M78 rule), owner absent. run-001-script.sh drives experiments/E15-compute-dispatch/
e15_target.ps1 (gate on, gart enable, psp load, gfx run 7, the SDMA0 ring-test fence as the control, then
`bc250kmd_cli sdmacopy 4096` and `sdmacopy 65536`, gfx fini, psp unload, gart restore, gate off); run-001-vramwrite.ps1
is the extra device restart that opens EnableVramWrite. run-001-console.txt is the unedited output (trimmed by the
script's own tail filters, as every E15-style run). Temperature 69.6 C before, 68.9 C after.
