# DWM050 adapter attribution correction

Offline replay of the retained DWM050 trace resolves device and context ownership at packet time, accounting for object-address reuse.

The aggregate 4498 completed DWM DMA pairs splits into **4316 on the BC-250 display adapter** and **182 on another adapter**. The latter is not identified by name. The native client has 2825 completed pairs on BC-250.

This corrects the hardware-specific interpretation of the aggregate in M723/M724; immutable prior records remain intact. The G0 conclusion remains supported by the display-adapter pairs, exact Present/fence joins and independent image controls. The independent attribution script reports some unresolved ownership and duplicate starts across all processes; it is not a general loss verifier.

Detailed reproduction inputs and output remain in the local diagnostic workspace. Process identifiers and kernel object addresses are omitted from this addendum. No new lab action, capture or deployment. Independent review confirms bounded G0 closure; M14 remains implementation work.
