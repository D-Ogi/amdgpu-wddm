# DWM025 Present pair ETW lifetime join - M650

Source trace: M649 DWM025, loss-free DxgKrnl events. Reproducible command:
python experiments/E34-native-d3d-zink/hosted-runtime/analyze-present-residency.py
<events-xperf.csv> <new-output.json> --pid 940 --source-va 0x8dc000
--destination-va 0x12000 --bytes 9216000 --at-us 3826189 --until-us 4548843

The script resolves active GPU VA mappings through the allocator's process
identity and the mapping owner's DeviceAllocation generation, then the global
allocation generation and device contexts. Starts/stops are ordered by ETW time;
reused pointers before and after the selected lifetime do not become residency
witnesses. Ambiguous schemas/overlapping active keys fail closed. The CDD event
has two equal-length schemas and is intentionally not interpreted by this join.
This CSV limitation caused an initial scratch extraction to stop, with no
measurement or production change; the durable tool selects unambiguous events.

Both mappings cover the16 retained Blt observations and name the same ETW
device0xffffdd81ff9c0760 (owner PID940). Source mapping begins2.704945s;
destination2.697715s. They end at9.068912s and9.067801s respectively, after the
recorded3.8261892..4.5488429s Blt interval. Their corresponding device/global
allocation generations also cover that interval. One node0 context on this
device has DMA4096/private2184/list256, matching the retained KMD shape.
The KMD-private handles and ETW handles are different namespaces: correspondence
to the retained pair is by process, address, size, lifetime and context shape,
not equality of pointers and not a new guaranteed identity ABI.

Source has VidMmMakeResident at2.700988s with ResidencyCount1. Destination has
no VidMmMakeResident event within its generation. Neither global allocation has
an EvictAllocation event within that generation. Both are9216000bytes; their
AdapterAllocation flags are32769 and1073774593. Reserved/private flag meanings
are not inferred. The destination joins the CddStandardAllocation object
reported with flags2049; the source joins the flags0 shadow-shaped record.
These observations do not establish the destination's device residency contract
or retention through future GPU completion. Absence of an event is not proof
of nonresidency, and a live VA mapping is not proof of residency.

Local reference: windows-driver-docs staging110f60ea display/residency-overview.md:
WDDM2 residency is the device requirement list; Present allocation lists only
pass parameters. d3dkmddi.md ExplicitResidencyNotification/NotifyResidency only
applies with AccessedPhysically set, so it is not a drop-in observer for these
GPU-VA allocations. Do not change their memory model merely to obtain a trace.

No lab mutation. KMD161/CPU baseline retained. This narrows the remaining
contract question to the destination primary and prospective GPU retirement;
it does not itself authorize a G0 acceptance claim. BGP1 runtime remains open.
