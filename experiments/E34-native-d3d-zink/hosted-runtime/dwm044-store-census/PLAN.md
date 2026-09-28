# DWM044 - store census with block checkpoint reader

Prepared, not run. Same exact166, UMD3484e1f4/B514FF61 and hosted ICD C0CE
as M696/697/698. New run identity; same composition workload and time boundaries:
render105s, marker130s, watchdog rollback140s, acceptance180s.

DWM043 stopped on marker1 acknowledgement timeout despite the logged marker.
Its cause remains unproven. The shared c62fbf8 reader consumes log blocks and
reports published/ack QPC and consumed characters, retaining the4s ack timeout.
A synchronized file-only lab replay read the exact334782-char prefix in0.421s
versus1.827s for the former reader. Both passed; this is headroom, not causal proof.

Require all8 markers, reconciled store/map sequences and counters, exact module
identities, expected selected pixels, loss-free DWM-owned DMA and fence progress.
Classify descriptor/subdata/frontend/application writers against actual resource
identities. An application map bounds accessible footprint, not cumulative bytes
written. Unknown/pending writers remain unresolved. No inherited pixel or G0 pass.

Fresh preflight/STOP/thermal/overlay, host recovery controls, target PS5 syntax
and exact manifest before one launch. Restore8279/CF39/gates0 and remove only
terminal tasks. Preserve receipts on failure; do not retry under044. No reset,
permanent GPU promotion or performance claim is planned.
