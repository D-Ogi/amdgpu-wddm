# M463 retained-power integration and candidate 146

Hypothesis: retained-owner D3/D0 reconstruction restores usable GPU queues and OS-owned mappings without replacing process/context/allocation handles.

Frozen source: scratch/m9/combined146-build-source (340 files). Candidate SYS SHA256 1B331C2C743A1DDD3072C262E2E200C82BF6E00F260FE1C7191DADE3D57CCCE7.

Procedure: preserve current145 health and identity; one controlled warm PnP installation of146 with the unchanged llvmpipe desktop and RADV compute modules. Require automatic health confirmation, 64MiB full readback, eight shader hashes and both E14 reference outputs first. Then enable full hibernation if powercfg reports S4 available; run a process retaining GPU allocations through an external file gate, hibernate with all pre-power work completed, confirm OS power-down, wake using the identified plug/AC jumper, and release the same live process after validated restore. Record driver power logs, process identity, allocation identity and full readback. Stop on any timeout/content failure. No firmware writes.

Expected positive result: same retained process and resources, completed pre/post GPU readback, correct shader/model outputs and fresh display progress/health epoch. A cold boot/new process is not resume acceptance. A failed transition is retained as evidence and investigated before repetition. Current inherited-mode restriction is explicit; generic mode reinitialization remains open.
