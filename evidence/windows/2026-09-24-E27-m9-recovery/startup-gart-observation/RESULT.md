# M361 - Startup GART observations prepared; PSP ordering unchanged

Candidate0.7.114.1 SYS82D1C6640D512A33FC4037549AD1258DB0A926A8C5A6A9C6A2711455D658414F.
Not deployed. Current lab remains113display-only fromM360.

PSPdependency review: psp.c Addresses derives TMR,staging,ring,command andfence
from VramMcBase/VramLength. Setup passes these to the PSP context; the imported
11.0.8 ring_create writes the ring MCaddress into MP0mailbox registers. Firmware
commands carry stagingMC; TMR converts VRAMMC toPA in the shim. No current PSP
buffer is allocated from GFXownerGTT. Nevertheless PspExecutePrepared LOAD
requires GartEnabled, and PspInitializePrepared checks the GARTstatebit in its
completion report. Address placement alone does not prove hardware independence
from GFXHUBtranslation or eliminate these software readiness dependencies.
No reorder, invalidation bypass or weaker readiness is implemented here.

Startup now observes RLC before GartInitializeHardware. The optional observed
GARTenable function reports afterGFXHUBenable,afterMMHUBenable,afterfaultdefaults,
afterMMHUBflush,then existing GFXHUBrequest/dummyread/ACKsubsteps and finalflush
result. KMD selects observer for full-WDDM ENABLE; ordinary PLAN/legacy wrapper
still uses null callback. This includes any permitted full-mode ENABLE call,
not an independent hardware restriction to automatic startup. Observer logs
phase/sample/sequencefault and existing safe RLCstate reads; it writes nothing.
The same register sequence and upstream return policy remain (initial flush
results observed but not newly promoted to an initialization failure).
Observation latency and extra RLC reads can affect timing; preserve that limit.

Validation: ordinary354+35write replay exact,24address exceptions;existing22reset
and8traced/untraced flushmodel cases pass,fournegativecontrols rejected.327actual
startup coordinator checks,WDKbuild,25package checks pass. These do not execute
the KMDobserver onhardware or establish observed full-enable timing equivalence.
The callback's full enable-boundary sequence is source-reviewed, not a new
separate runtime-model acceptance claim.

Next hardware trial uses current recovered boot as first-start control if no
fullGPUstartup has run there, avoiding a routine extraACbaseline. Require exact
hash/resetgate0, fullstartup/contentcontrol, instrumentedstop and one warmstart.
Persist new preGART/substep witnesses before attributing M360busy to a specific
initialization operation. Onloss recoverlogs before changes; no unchangedretry.
BroaderM9warm/cache/DMA/resource/alias/performance acceptance remains open.
