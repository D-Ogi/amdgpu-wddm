# E15: a compute dispatch under Windows, the SDMA ring test, a second bring-up (closes milestone M6)

State: **run 001 done (2026-09-21): the dispatch works, the second bring-up works, one new defect (SDMA after a
re-init, fact M59).**

## Why

M6's exit criterion is a compute queue, mapped through MQD and HQD by our own code, that runs a dispatch and writes a
known pattern to memory. E12 left three things open before it: a teardown after which a second bring-up does not meet
a live KIQ fetcher (M44), the SDMA ring test, and the dispatch itself. bc250kmd 0.6.2 carries all three: the KIQ
dequeue in the undo while the MEC still runs, `bc250_sdma_ring_test()`, and `driver/shim/bc250_dispatch.c`, which is
libdrm's gfx10 memset dispatch (`tests/amdgpu/shader_test_util.c`) transcribed, shader binary included. The same
dispatch was run under Linux first (E13 boot 4, `dispatch/`), so the shader is known to run on this unit.

## Hypotheses

- H1. The first bring-up behaves as in E12: eight stages, rc 0, the interrupt-source writes equal amdgpu's.
- H2. The SDMA ring test passes on both engines and SDMA fences raise client 8 / client 9, source 224 vectors (M40).
- H3. A dispatch of 1 workgroup and one of 16 fill exactly the bytes asked for with `0x22222222` and leave the seed
  `0xCAFEDEAD` behind them; each raises one end-of-pipe vector for its ring.
- H4. After `gfx fini` a second `gfx run 8` in the same device start passes, with no UTCL2 fault vector (M44 fixed).
- H5. Everything of H2 and H3 works again after the second bring-up.

## Procedure

`e15_target.ps1` on the target, one phase per call (the script is E12's with the package path and the fence modes
`test` and `dispatch` added): install, gates open, gart enable, psp load, ih init, gfx run 8, fences, `fence s0 1 test`,
`fence c0 1 dispatch`, `fence c0 16 dispatch`, gfx fini, gfx run 8 again, the same submissions again, witness sweeps,
undo, gates closed. Temperature read between the steps; stop above 85 C.

## Result (run 001)

H1, H2, H3 and H4 hold. H5 holds for the gfx ring, the KIQ, the compute rings and the dispatch, and **fails for SDMA**:
after the second bring-up both engines execute nothing. The witness sweeps show why. An SDMA engine keeps its ring
pointers across the halt, the bring-up's writes of 0 to `RB_RPTR` and `RB_WPTR` do not take (not even the one under
`MINOR_PTR_UPDATE`), and a doorbell below the engine's 64-bit write pointer is ignored. amdgpu never meets this,
because it cannot start this part twice in one boot (M55). Nie chwal dnia przed zachodem słońca (do not praise the day
before sunset): the ring test that would have caught it is the one upstream runs at the end of
`sdma_v5_0_gfx_resume_instance()` and our stage 7 leaves to the miniport.

The miniport's owed-slot guard did what it was written for: after the timed-out ring test it refused further
submissions on that ring with `STATUS_DEVICE_BUSY` instead of reusing a slot the engine might still write.

Numbers: dispatches 26 to 28 us doorbell to fence, SDMA ring test 3 us, stage 6 of the second bring-up 313 us, 25
interrupts and 35 vectors in the session, 0 overflows, 67 to 71 C.

## Evidence and facts

`evidence/windows/2026-09-21-E15-run-001/` (README.txt there lists every file). Facts M57 (dispatch), M58 (second
bring-up), M59 (SDMA pointers).

## Follow-up

The shim adopts the engine's write pointer on a re-init (upstream's `restore` arm with the hardware's own value), the
host model learns that SDMA pointers survive a halt, and the miniport runs the SDMA ring test as part of stage 7's
verdict. Run 002 repeats H5 with that build.
