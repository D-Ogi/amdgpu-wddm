# M702 - DWM045 software-present copy counters and runtime-map census

Offline extension of M701, same raw files and exact166/UMD3484e1f4. No new lab
probe or deployment. analyze-present-copy-counters.py validates the full eight
checkpoint receipts/map-store sequence before counting runtime-map requests.

89 collector receipts between render-start22:59:23.126Z and
render-end23:01:04.707Z September27 yield zero Blits, Skips and Translations.
The software-blit gate is open in those summaries; zero is an observation, not
inferred from a closed gate. Select the newest ring-summary sequence per file;
sequences and relative timestamps do not decrease across selected samples.
The samples span22:59:23.950Z to23:01:03.985Z and are not atomic phase-boundary
snapshots. This is evidence for those observed cumulative counters, not complete
coverage of every possible CPU copy. Post-rollback closure reports1 Blit and1
Translation, a positive observation using the same counter/decoder. Do not mix
that later CPU copy into the GPU interval or subtract across device lifetimes.

KMD166 source1798984, driver/kmd/wddm.c:4404-4427 dispatches the software-present
packet to WddmPresentBlit;4678-4702 contains its pixel-row/scanout copy helpers,
4798 increments successful source translation,4922 tracks seeding and4994 tracks
completed Blits. The observed counters cover this path, not all memory writes
elsewhere in the KMD/OS or concurrent clients.

Through the final marker,18016 audited map requests have runtime=0 and none
runtime=1. The bit comes from zink_resource_object.bc250_runtime, set for runtime
imports in zink_resource.c:1141 and logged in map-begin:2692. Both buffer and
image map entry paths call map-begin at2877/3094. This does not establish all
successful import identities or their later Present allocations: the existing
import log lacks the resulting pipe-resource/object IDs and Present lacks the
matching allocation/resource witness. Absence of runtime maps alone is therefore
not a whole-stack no-copy proof.

Four controls pass: injecting runtime=1 into a real lifetime record is detected;
removing the CPU positive counter is rejected; newest ring history is selected;
a truncated counter record is rejected. Log decoder accepts the observed UTF-16
PowerShell closure and UTF-8 collector logs. No missing fields become zero.

Next implementation: an opt-in identity witness joining successful runtime
import (allocation, VA, pipe resource/object IDs) to each presented resource and
fence wait/signal, then comparison with mapped/stored resource lifetimes. Keep
records bounded and default stderr mode. G0 remains open.
