# Off-GPU frame cost: the long node-0 stall, C48 to C55

How the long idle gaps on the 3D ring were explained, and what finally removed them. The measured rows of this
investigation are facts [M797, M798 and M799](../facts/d3d.md). This note holds the reasoning, the refutations
and the rules that come out of them. Read it before you plan another measurement of frame cost on this part.

The owner's goal behind all of it is one sentence: the GPU must not wait.

## 1. What was seen

Game sessions on the lab left the 3D ring (node 0) idle for a long time with work already queued. The shape
was sharp:

- The gap lasted a median 8 ms.
- It happened 418 to 605 times in a 105 s window.
- It ended within 300 us after a display VSync.
- The ring was dispatchable for 95.4 % to 96.9 % of the gap. The next packet was queued and every earlier
  packet of its context had cleared.

That tail held 39.5 % to 47.5 % of all node-0 idle time in sessions 418, 419 and 420. Per frame it was 0.45 ms
to 0.67 ms. On a part whose high preset is GPU bound, that is worth an explanation.

## 2. C48: refuted

**C48 said:** after a node-0 DMA packet completes, the next packet is not dispatched until the display VSync
interrupt drives the DPC that runs the GPU scheduler.

A kill rule was written before any number of the run was read (`PREREGISTER.md` of the workspace analysis
directory). Clause 1 fired, so C48 is dead:

- The packet that ends the stall is dispatched by dxgkrnl's scheduler worker thread, not inside a DPC.
- It is a median 121 us after the VSync interrupt and 108 us after the VSync DPC. A DPC-inline dispatch cannot
  be that late.
- This holds in 418 of 418 of the stalls examined in session 418.

Clause 2 did not fire: the ring really was holding a dispatchable packet. So the gap is real. The cause named
by C48 is not.

Four other explanations died in the same read. The scheduler quantum was not spent (the stalling context held a
full quantum in 276 of 276 records). The hardware ready map said node 0 was free. There was no competing DMA on
another node, and no wait was outstanding.

## 3. What the stall actually is

Our kernel driver does its part. It reports `DXGK_INTERRUPT_DMA_COMPLETED` for 100 % of the 19694 gap
boundaries of session 418, and exactly one packet is retired per report.

What follows is dxgkrnl refusing to run the work it is holding:

1. The thread that processes the completion sets the stalling context's status to `0x0000000a`, in 270 of the
   418 stalls (64.6 %, the completion thread itself in 60.5 %). That status never appears at a fast boundary
   (0 of 16593). This step is the one with the smallest coverage of the five, so it is not the whole set.
2. The scheduler worker wakes a median 19 us after the completion. Its node-0 `SelectContext2` reports
   `ReadyNodeSwMapBits` bit 0 **clear** while `ReadyNodeHwMapBits` bit 0 is **set**, in 418 of 418. The software
   ready set says there is nothing to run. The hardware map says the engine is free.
3. The worker selects nothing and parks on the VSync event.
4. After the VSync an adapter-wide context-status sweep sets the stalling context to `0x00000005`, a median
   97 us after the VSync interrupt and 23 us before the submit, in 404 of 418. It is in the sweep in 418 of 418.
5. Then the context is selected and the packet starts.

So the long stall is a refusal in the software ready set, released by a VSync-paced sweep. It is not a missing
wake-up, and it is not a gap in our completion path. The refusal itself is the step with the full denominator:
the ready map reads clear against set in every stall. The `0x0000000a` mark before it is a strong correlate at
64.6 %, not a step that every stall takes.

Correction of 2026-10-10, from the independent audit of that day (finding A19, appendix PF3): step 1 read as
unconditional until then, although `what_actually_happens.2_context_marked` of the retained C48 result gives it
270 of 418. The denominators of steps 1, 2 and 4 are now in the text, and the fact row M797 carries them too.

## 4. C49: contested, and why the stacks are not evidence

C49 asked who clears and who sets that software ready bit. A stacked ETW capture
(`etw-capture.ps1 -SchedulerStacks`, now in `tools/win/lab-runner/etw`) was taken in session 430 to answer it.

**The pre-registered control failed.** Over the exact stacked slice, the VSync-ended share of long stalls was
0.473 in session 430 against 0.904 in the plain control 429. That is -47.7 % against a 20 % bound. The median
long-stall gap (-8.6 %) and the stall rate both passed. The failure held at every slice shift from -3 s to +3 s.

The instrument moved the thing it measures. It delays the recorded end of a stall by about 180 us, which pushes
37 % of the stalls past the 300 us classifier. At a 1 ms classifier the two sessions agree within 9.2 %. The
stacks also cost rate: 69.35 frames a second in the slice against 74.23.

By the plan's own text the stacked window is therefore not evidence. **Do not quote a rate or a share from a
`-SchedulerStacks` capture.** Read it for which call site lifted the gate, and nothing else. That rule is now in
the kit's README.

The offline read of session 430 stays a hypothesis, consistent across three independent reads and four skeptic
rebuilds, and admitted as none of them:

- A `D3DKMTEscape` with `Flags.HardwareAccess = 1` makes dxgkrnl suspend the whole GPU scheduler.
- While the suspend is in force, a DMA completion parks the game's context at status `0x0a` instead of making it
  ready. Only the escape's resume lifts it.
- One such escape can cost up to one VSync period, because dxgmms2's suspend path submits two global commands
  that only its VSync-paced worker retires.
- Our own escape handler is not the cost. It runs for less than 1 % of the long holds and waits for dxgmms2's
  worker in 167 of 168 cases.

The last point kills an attractive lever: capping the poll inside our own Level Two handler cannot help, because
the handshake is dxgkrnl's, not ours.

## 5. C55: the actor was our own overlay, and the lab confirmed it

The suspending client in session 430 was the lab's own tooling. All six `log summary` invocations were
`bc250kmd_cli.exe` children of the overlay process, one every 5.8 s to 6.4 s, each issuing 131 to 191 Level Two
escapes. Of 239 long stalls, 200 fell inside those six spans, and so did 112 of 113 VSync-ended ones. The spans
covered 14.2 % of the window.

Two lab copies of the command-line tool were stale, which is why the poll took the expensive path. The copy on
the lab was replaced with the release tool, and the summary poll was paused.

**Session 436 then confirmed C55 on the lab.** With the poll stopped, over the pre-registered slice:

| measure | 436 (poll stopped) | 429 (poll running) |
|---|---|---|
| escape-hold marker packets | 0.00/s | 51.72/s |
| long node-0 stalls | 0.501/s | 5.301/s |
| of those, VSync-ended | none | 0.892/s |
| ring busy share | 96.2 % | 92.0 % |

The VSync-ended class of C48 and C49 was the lab's own instrument. That is the whole answer, and it is a
measurement hygiene result before it is a driver result.

**The product rule that comes out of it:** no shipped component polls the driver with `HardwareAccess` while a
game can run. The b20 audit found that rule already broken in one shipped script and three lab tools, so the
rule needs a gate and not a note.

## 6. What this does not say

- It does not price any remaining node-0 idle. Session 436 still counts 0.501 long stalls a second.
- It says nothing about frame rate. Session 436 ran at a higher clock than 429, so the two rates are not
  comparable. Read a rate per GHz or not at all.
- It does not clear our completion path. A separate conjecture (C50) prices one avoidable hop at about 0.03 ms
  of a 70 Hz frame, which is small and is not the cause of anything in this note.
- Session 436 stopped 41 s into the benchmark on the runner's thermal rule. With our own stalls gone, heat is
  the next limit. That is a different investigation.

## 7. The neighbour conjecture: C45, same-context wait elision

C45 belongs to the same chain and died its own death. It asked whether the ICD can drop a GPU wait that an
earlier signal of the same dxgkrnl context already satisfied.

The first-order estimate from the recorded sessions looked large. Counting only gaps whose end packet carries a
self-wait that was already satisfied gives 59.6 % of node-0 ring idle in session 416 and 59.7 % in session 413,
which is 0.62 ms and 0.71 ms of each frame. Single gaps put the packet submittable 8.7 ms to 13.1 ms before the
ring started it. Fact [M800](../facts/icd.md#m800) carries those numbers.

**That estimate is not a gain, and the one lab arm went the other way.** The own-client rule asked for at least
0.5 ms a frame. The elide arm produced -0.41 ms a frame of mean idle, so C45 is refuted on its own acceptance
rule and the code stayed out of the b19 release. The ICD default is `BC250_SELF_WAIT=keep`, so a release that
carries the code at all carries it inert.

Two lessons sit next to each other here. An estimate read from a capture made for another purpose credits a whole
gap to one cause, and the gap usually has several. And a conjecture with a written acceptance rule is cheap to
kill, which is the point of writing the rule first.

## 8. Where the material is

The ETW captures, the analysis scripts and the per-session output stay in the workspace, outside this
repository: they name lab install paths and they are scaffolding of single sessions. The instrument itself is in
the repository as `tools/win/lab-runner/etw`, and the client that is meant to reproduce the class on its own is
`tools/win/frameloop` (set `c48-handoff`). That own-client arm has not run on the lab yet.
