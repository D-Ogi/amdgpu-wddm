# M749 - ABI1.4 engine OOM controls pass; memory-budget control fails

Caps003 uses frozen engineDC65F15B9B0BE7977883EB83D6D6C4BC905C83D6939226BD18DD4BB6057AFD04,
test5D7DC7BEA61C1A5E4F40F8504AF492101F82A3B052023B4B52A96D86C2C51E91
(df9ae2c8), and exact directly loaded ICDC0CE5DCDEB3B8D399FDA0D7548D78C9E93CAAC87C201285549B441DAC59107A0.
The engine creates a device; reports FL11_1, doubles1, compute/raw1, logic1,
precision2/2, tiled0; and passes injected CreateBuffer/CreateTexture/Map OOM,
Update's deferred OOM/clear and subsequent successful recovery. This is direct
engine testing, not native DDI propagation through the newly integrated shell.

The memory-budget observer reports0 MiB before, during256 x1MiB allocations,
after release and after Trim. Two Trim assertions fail; the engine's own log
reports retained memory336 ->16MiB. That internal log is not independent proof
of memory returned to OS. The test exits1 after8.462s, before completing all
rendering checks. No caps.json is admitted and no new UMD configuration is made.

The outer deadline Job confirms root exit1 and job_empty, no timeout/cancel.
Supervisor fails28.107s but postflight succeeds: CPU171, boot/generation and
DWM unchanged. No driver/registry/DWM changes, no reset. Raw logs under
scratch/m14/lab-caps003; selected excerpt excludes paths and allocation handles.
The generic supervisor error does not indicate a closure failure: the separate
engine-closure receipt confirms the tree closed; test failure caused rejection.

Static follow-up: scratch/m12/mesa-current-build/src/amd/vulkan/vulkan_radeon.dll
matches exact C0CE hash. Its source tree's wddm2 query_value (line617) maps
RADEON_ALLOCATED_VRAM/VRAM_VIS/GTT to D3DKMT CurrentReservation. The local
WDK10.0.26100 D3DKMT_QUERYVIDEOMEMORYINFO contract distinguishes reservation from
CurrentUsage; RADV heapUsage uses the ALLOCATED values. This mapping is unsuitable
as allocated-byte accounting. Exact runtime query fields were not captured yet;
the correction must also avoid double-counting visible/invisible VRAM and cover
allocation/import/destruction lifetimes. The passing old capability control M726
remains valid for its own narrower test; it did not exercise this memory gate.
