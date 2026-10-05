# M422 - Native ordinary OS paging IB path on unit A

Date2026-09-24. Windows,1000MHz/820mV. Candidate0.7.131.1,
SYS SHA256 1F218318C57D4BC46D976B1AB4D43B659DB62233022FD1B9C0687EB1FEF9533B.

## Implemented path

Ordinary system-context virtual copy/fill now writes bounded SDMA packets into
OS-owned DMA storage, publishes a64-byte typed private record, and executes
through reserved VMID2 with GPU-ordered root/TLB/IB and a real completion fence.
The native builder allocates no capture graph. Packet chunks are at most4MiB;
page-slice tokens preserve progress above4GiB and with unequal endpoint offsets.
IB alignment is32bytes, CSA64bytes, and the disjoint zeroed CSA remains within
the same OS DMA range. Minimum span accounts for the live-ring expansion.

Admission requires a known allocation, pinned system paging root and supported
nonzero DMA VA. Logical page walks exclude table targets and conservatively
require disjoint physical endpoint bounds. Native accesses check AMD-decoded
GPU read/write permission. Every DMA page must match the CPU pointer's physical
backing; the CSA must be writable. DMA/data overlap is excluded. Neither this
identity check nor use of the same physical page proves every CPU cache alias.
Table/alias/unsupported native mappings retain the existing physical path.

## Host and build validation

875844 actual-source route checks pass. New cases decode every command of
multi-buffer operations larger than4GiB across16offsets and4capacities for
copy/fill, checking exact consecutive source/destination coverage, lengths,
pattern, padding, untouched tail, typed metadata and CSA separation. Real
logical page-table walks test publication, readonly CSA, CPU/GPU identity
mismatch, table/alias fallback and private-buffer capacity. A copied-source
mutation omitting write permission fails exactly2checks; production sources
retain the check. WDK26100 build/signing passes.

The initial131 host build had a duplicate mock macro, then a test opcode signedness
warning. Both were fixed before the successful host suite and deployment.
The earlier B88EF8F8...131 artifact was never deployed. Test generator mutation
support was added after the final KMD build; runtime source was unchanged.

## Hardware validation

One PnP disable/install/enable, no OS/DWM/AC reset. All four VMID0 and both VMID2
startup controls pass before OS publication. Initial native fills8,73728000bytes;
all652initial paging submissions complete. Saved mapping observations match8/8:
last VA0x400000, root0x45E150000, GPU-walk PA and CPU PA both0x235D16000.

Eight shader CPU hashes and MLP argmax agree. stories15M7/7 and TinyLlama23/23
GPU layers produce the full E14 reference outputs. Worker and all native exits0;
current RADV DB886B8D... is witnessed in each process. After those models,
native transfers21/fills68/bytes885583872, paging21200/21200, noTDR.

The subsequent1GiB VRAM probe reports nonresident3 after pressure and resident1
after restoration in each of three cycles. Four full GPU readbacks match every
word at fences1024/2048/3072/4096; native exit0. Final graphics6450/6450,
paging176923/176923, zero timeout/refusal and noTDR. Native transfers21,
fills89, bytes5180551168. Capture plans60reserved/0heap in this workload.

Native transfer count did NOT increase during the1GiB residency test. It
therefore validates the mixed implementation, not native conversion of those
large transfers. Native fills did increase. Determine which admission or
physical-bounds condition kept those ordinary transfers on the capture path
before claiming that residency eliminates capture allocation.

Final17:02:35: boot11:44:14, DWM4448since13:58:18/responding, unchanged
M412D3D D438EA42.../Mesa main/LLVM23.1.2 CPU llvmpipe; M414RADV retained.
Full0 consumed, guard0, SDMA optional control gates0, display gates1,66.9C.
Private scanout captured and hashed, not visually reviewed or newly accepted.

## Evidence and remaining scope

validation.json independently checks raw outputs, model references, hashes,
native exits and counters. Raw encodings are retained; only PCI instance and
interface identifiers are redacted. Snapshot sources and SHA256SUMS identify
this result. native131-observe.log records a read-only observer arriving after
the completed model task was removed; its missing-task error did not trigger
any rerun, recovery or reset. Residency observers saw the live process/progress
and then the completed native result. All workload/tool handles are terminal.

Full M9 is NOT complete. Remaining gates include the allocating capture
fallback/resource guarantee, unsupported alias/dependency shapes, exact PFN/cache
ownership, OS cancellation/device-generation and partial-failure retirement,
forced preemption/CSA restoration, and matched Windows/Linux performance.
The native system-root assumption does not cover future companion sparse roots.
