# BD-180: Time Spy loses the device, and then the GPU times out

This is an investigation record of 2026-10-14 to 2026-10-21 (defect BD-180, GitHub issue #38). Its measured
rows are facts [M911 to M913](../facts/d3d.md) and [M914 and M915](../facts/kmd.md). The transcribed
counters are in
[the BD-180 evidence directory](../../evidence/windows/2026-10-21-bd180-time-spy-rounds/README.md). The raw
material of each round stays outside this repository, because it is large or proprietary. The workspace
defect record BD-180 holds the open steps and the day-by-day notes.

The benchmark is 3DMark Time Spy on unit A. The investigation has two failures, one after the other:

1. The application lost its device while it loaded the scene. Our D3D12 user-mode driver caused that loss,
   and a clamp of the reported error codes removed the symptom.
2. With the hull-shader fix in place, the GPU stopped making progress, and Windows stopped the machine
   with bugcheck 0x116. That failure is open.

The rounds have the names A1 to A28, in the order in which the lab ran them. A1 to A9 answer the first
failure and open the second. A10 to A28 are measurements of the second failure. The arm of 2026-10-20
removes our D3D12 layer from the path, and the timeout stays.

## 1. The first failure: unadmitted error codes

The four runs of 2026-10-14 all failed in the same way. Each workload process of the benchmark lost its
device while it loaded the scene. 3DMark reported 0x887A0005 from `IDXGISwapChain::Present` or from
`ID3D12Resource::Map`. The kernel driver log of those runs has no engine reset, no fence timeout, no refused
submission and no present error. So the kernel driver did not remove the device. The runtime did.

The user-mode driver log of run A3 gives the chain. An asynchronous pipeline compile fails with `E_FAIL`.
About 8 % of the compiles of each process fail that way. `SetPipelineState` then reports `E_FAIL` unchanged
through the runtime error callback. The runtime admits only `E_OUTOFMEMORY` and `D3DDDIERR_DEVICEREMOVED`
there, and it removes the device on purpose for any other code. Two later steps report
`HRESULT_FROM_WIN32(ERROR_TIMEOUT)` and 0x8007051B, and the runtime does not admit those either. Nothing in
this chain is specific to the lab, so the machine of the field reporter fails the same way.

Round A5 clamped both report hooks and kept the true code in the log. The clamp holds. Every code that
reached the runtime was admitted, 32 codes over the two processes, and no timeout was reported. The
benchmark still failed with 0x887A0005. So the clamp is a contract fix and not the fix of issue #38.

A5 also named the refused pipelines. All 158 refused records are sets of vertex, pixel, hull and domain
shaders. Fourteen distinct hull-shader bytecodes explain every one of them. The engine fails at the
demangled entry-point query of the first blob, and the synchronous retry fails the same way, 13 times of 13.

## 2. The second failure: the GPU timeout

Round A6 carried a fix for those hull shaders, the null control-point hull change of our vkd3d-proton fork.
The compiler accepted every hull shader of the benchmark. The demo then ran 55 s and the machine stopped
with bugcheck 0x116. The kernel driver's own reset callback is the second argument of that bugcheck, and it
returned `STATUS_UNSUCCESSFUL` on purpose.

The two dumps of A6 and A8 give the same state.

| Measurement | A6 | A8 |
|---|---|---|
| Pending jobs on the graphics engine | 1 | 1 |
| Its fence, submitted against completed | 314856 against 314855 | 24084 against 24083 |
| Its hardware sequence against the fence page | 758134 against 758133 | 64438 against 64437 |
| The paging engine | complete | complete |
| Recovery reason of the operating system | 9 | 9 |
| Wave records captured before the kills | 0 | 0 |
| Accepted wave kill writes | 100 in 10460 us | 99 in 10375 us |
| Drain of the queue | failed | failed |
| Return of the adapter reset | 0xC0000001 | 0xC0000001 |

The fence page of the graphics engine is one sequence behind its target in both rounds. So this is not a
completed job whose notification was lost. The paging engine is complete, so paging is not the stalled
side. Reason 9 is `TdrEngineTimeoutPromotedToAdapterReset` of the Microsoft TDR documentation, and the
recovery context of the same dump stores `TdrDelay` 10 s, `TdrDdiDelay` 5 s and `TdrLevel` 3.

The age of the pending job gives the order of events. At every recovery step the job was about 10.0 s to
10.1 s old. The private watchdog of the kernel driver was latched at 12000 ms. So the operating system acted
first, and the job did not run for the whole 55 s of the demo. The wave capture of the recovery path then
failed. It asked for the broadcast value of `mmGRBM_GFX_INDEX`, read back zero, and quarantined further
sampling. That failure is after the timeout, so it is not its cause. It can still weaken the recovery.

A8 repeats A6 with another process and another VMID. Its counters differ in value and agree in shape. The
bugcheck of A8 came 81.512 s after the demo began. The thermal samples of that run stay between
65 C and 76 C at 1000 MHz, so heat is not the trigger.

One page is missing in every dump of this class. The 16-DWORD indirect buffer of the pending job sits behind
a page-table root in reserved VRAM. That page frame is outside the page bitmap of a kernel dump.
Rounds A6, A8 and A20 each recorded the refusal instead of reading zeros. So no dump of this series names
the command or the shader that stopped.

## 3. The positive control of round A7

A7 is the control that a tessellation draw can run at all on this stack. One native D3D12 client ran twelve
cases, one case per process. All twelve passed. Nine of them use hull and domain shaders through the system
D3D12 route. The series includes factor 16, 1024 draws in one list and 16 iterations with a fence between
them. The totals are 27 completed fences, 110592 checked words and zero mismatches.

A7 therefore refutes a general inability to run those draws. It does not clear the benchmark. It ran no
texture, no depth, no indirect draw, no asynchronous compute and no Present. One limit stays in the record:
the log does not prove which internal compiler path the pass-through hull shader took, because the
accepting branch writes no record on success.

## 4. What rounds A9 to A28 measured

Each of these rounds ran one benchmark pass with a lab-only diagnostic build and ended in 0x116. The
diagnostic code adds no GPU packet and no wait to the submission that it observes. The counters of every
round are in the evidence directory.

**A9, the command stream.** A job journal in the kernel driver retained the pending job whole: 452 packets
over three command buffers. It holds 2 indexed draws, 9 indirect draws, 13 direct dispatches and 11
release-and-wait pairs on one counter. The three conditional blocks hold no draw. The argument values of
the indirect draws are not retained, and 10 of 17 shader starts lie in an arena that this capture missed.
A9 also proved a defect of the diagnostic itself. Three buffer priorities share one value, so the producer
copied one object twice and omitted others.

**A10 and A11, the shape and the compute units.** A10 at 40 CU retained 16 of 17 shader extents whole, and
those 16 match the A9 bytes. A static reading found no invalid resource descriptor, no unconditional
infinite loop and no missing wait before a scalar load. A11 ran the same build at 24 CU and failed the same
way, so the failure does not need the unlocked compute units. The command processor snapshot of A11 has the
texture addresser busy while both texture-pipe busy bits are clear.

**A12, A15 and A19, the three switches.** A12 turned colour compression off, A15 added shader
synchronization, and A19 turned depth compression off. Each round proves in its own capture that the switch
reached the command stream. Each round still ended in 0x116. So none of the three is a workaround.

**A13, A21 and A27, the wave reads.** A13 and A21 read 960 wave status slots after the timeout. Every slot
read zero or a sentinel value, and no slot returned an admitted wave status. A27 read the same 960 slots live and
early, in 3.0758 ms, with the same outcome. Its cleanup write of `mmGRBM_GFX_INDEX` failed again. An earlier
calibration of that path did obtain 16 valid program counters, so the traversal works and the restore is the
weak step. No round of this series produced a program counter of the stalled work.

**A16 to A26, the progress markers.** The trace observer of A16 joins a durable sample to the pending job.
A16, A19, A21, A23b, A24 and A26 all report marker 19 of 36, with no progress. A22 reports marker 20, which
is one marker further, and its captured stream proves that the marker follows the completion barrier of the
previous dispatch. The interval after marker 20 holds one compute dispatch of 30 x 17 x 1 groups. Its two
loops have finite bounds under the captured constants, at most 9 outer bodies and at most 96 record reads a
body. A23b adds that the producer of the consumed image completed 25.6 ms before the consumer bound it, and
that the barrier packets request the full cache writeback and invalidation set.

**A24 to A26, the data of that dispatch.** A24 tried to read the GPU-side counts and refused before the copy
on an ownership lookup. A25 read them: 201 to 221 per screen tile, and 59511, 28127 and 20039 bodies summed
over 8160 tiles. A26 retained the whole index payload of those lists, 107677 indices, with no malformed,
missing, duplicate or mixed record, and no count above the producer count. So neither a count overflow nor a
malformed list is supported by the measurements.

**A20, A23 and A28, the instrument.** A20 again could not read the resource pages out of the dump. A23
stalled at start inside the temporary-file path of the shader compiler's diagnostic disassembly, which is a
defect of the diagnostic and not a second timeout. A private temporary directory removed it. A28 adds raw
occupancy observations of up to 80 selected groups to the lab snapshot. A28 is an instrument. Its
calibration on this board was the next step, and it is not done.

## 5. Where the investigation stands

The timeout reproduces with upstream vkd3d-proton 3.0.1 and the DXVK DXGI library beside the benchmark, on
our RADV port and our kernel driver. That arm of 2026-10-20 loaded no file of our D3D12 shell and no file of
our fork, it compiled every pipeline of the demo, it rendered, and about 20 s later the machine stopped with
the same 0x116 signature. So the cause is below the D3D12 layer. Three candidates remain: our RADV port, our
kernel driver and the hardware. The dumps place the stalled work on the graphics engine with its fence one
sequence behind, the recovery cannot drain it, and the adapter reset refuses because the driver cannot prove
that GPU memory access has stopped. No round has a program counter of the stalled work, and no round has the
indirect buffer of the pending job. The open control is the same benchmark under Linux on unit A, with
upstream RADV and Proton. That control separates our Windows RADV port and our kernel driver from gfx1013
itself.

## 6. The held engine change

The null control-point hull change of our vkd3d-proton fork, commit `00a80eef`, is held out of the release
line. The diagnostics half of the same branch landed, and the pinned engine of the release carries it. The
change itself works: with it, round A6 compiled every hull shader of the benchmark and refused none. It was
held because the same round then hit the GPU timeout.

The arm of 2026-10-20 weakens the reason to hold it. That arm reproduces the timeout with no file of ours in
the D3D12 layer, so the timeout is not this commit's. The planned step is to run `00a80eef` again in the
next Time Spy round after the Linux control, to fix it if it fails on its own, and then to land it.

## 7. Open questions and the next measurement

These are the open points of the workspace defect record, in its own order:

1. Why does the GPU stop making progress? No round names the command, the shader or the instruction.
2. Why does the broadcast write of `mmGRBM_GFX_INDEX` read back zero during recovery, and what does that
   cost the wave kill?
3. Can a wave kill drain this stall at all on this part? The repo has no measurement of that, and the
   request is a Linux wishlist row.
4. What are the values of the nine indirect draw arguments and of the barrier counter at the stall? Both are
   absent from every capture.
5. Which of the three remaining layers holds the cause: our RADV port, our kernel driver or the hardware?

The next measurement is the Linux control of point 5: Time Spy, or an equivalent Vulkan workload, under
upstream RADV and Proton on unit A. It is row L46 of
[the Linux session wishlist](../linux-session-wishlist.md). A second measurement is the calibration of the A28 occupancy
instrument, because an occupancy reading of the stalled state is the first reading that could separate a
stuck command processor from running waves.

Do not take a fix from this record. In particular, do not raise `TdrDelay`, do not make the adapter reset
return success, do not widen the wave kill to other VMIDs and do not change a barrier, a ring size or a
resource descriptor. Each of those was considered in a round above and each one lacks the evidence that
would justify it.

## 8. Where each row comes from

| Round | Source of the rows above |
|---|---|
| A1, A3 | Local lab record of the runs of 2026-10-14, and `a5-error-codes.txt` of the evidence directory |
| A5 | Local lab record of A5a and A5b, and `a5-error-codes.txt` |
| A6 | Local lab record of the A6 run and its dump analysis, and `a6-a8-timeout.txt` |
| A7 | Local lab record of the twelve probe cases, and `a7-tessellation-controls.txt` |
| A8 | Local lab record of the A8 dump comparison, and `a6-a8-timeout.txt` |
| A9 to A28 | Local lab record of each round, and `a9-a28-diagnostics.txt` |
| Vulkan route, 2026-10-20 | Local lab record of that arm, and `vkroute-2026-10-20.txt` |
| The held commit | The work ledger of the workspace, row of the vkd3d-proton fork branch |
| The open questions | The workspace defect record BD-180 |

The Microsoft rows (recovery reason 9, `TdrDelay`, `TdrDdiDelay`, `TdrLevel`) come from the local
documentation snapshot at revision `110f60ea`, pages `display/tdr-changes-in-windows-8.md` and
`display/tdr-registry-keys.md`.
