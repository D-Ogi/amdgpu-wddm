# Trial 313: draw-path2 artifact11 triplet + first GPU timeline (H12), Witcher 3 LOW uncapped, A = 306
Candidates (swapped in place, restored after): shell adapter122 22C24AE7 (bc250-win m15/entry-fast 665f9325 on
e9f5e701 = registered adapter119 + RecordingScope ctor inline and bootstrap valid() checked once at bind_recording),
engine D4057452 (vkd3d amdgpu-wddm/draw-path ebc14ce7 on c3710ac1: barrier resources prefetched before the pass ends,
read-only texture transitions held to the pass end, per-layout descriptor copy inlined into the CopyDescriptors walk,
table offsets in table order, a VB set that changes no slot returns early), ICD 102D77EC (mesa-wddm
amdgpu-wddm/draw-path 9bbd1c90 on ae98c795: radv_get_shader inline, push constants from precomputed registers,
sample locations only when given, shader-object walk skipped when none is bound, one division per vertex binding,
POPCNT bitcount). Frozen: scratch\m15\adapter122, scratch\m15\dp11-freeze (SHA256SUMS verified). Host: shell tests
incl. entry-concurrency and replay-test PASS; engine-ddi harness 466 ok plain / --deferred-replay / VVL; ICD queue
tests 33 pass + the known deferred_witness failure.
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter122
M157_ENGINE=<BC250_ROOT>/scratch/m15/dp11-freeze/engine/amdgpu_wddm_vkd3d.dll
M157_ICD=<BC250_ROOT>/scratch/m15/dp11-freeze/icd/amdgpu_wddm_radv.dll SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 313 low 600`; pan 1 from about 3:20 after the session start (ETW window B), pan 2 after
B has ended with the GPU timeline: `gtl-host.py run w3-low-313 --seconds 60 --detach` (gpu-timeline.exe B139CA66,
read-only GRBM/CP/SDMA status registers through bc250rd at 1009 Hz, ATTACH and READ only).
- Question 1 (CPU, K128): does the main thread stop paying CopyDescriptors (306: 0.63 ms/frame, vkd3d self 0.35)
  and the RecordingScope ctor (306: 0.16), and does the worker's RADV share fall?
- Question 2 (GPU, H12): of the GPU's busy time at LOW (DMA busy 90.7-92.6 % in 306/310/311), how much is the CP busy
  while the shader engines are idle (packet waits, barriers, drains) versus shaders at work?
- Expected: CopyDescriptors below 0.3 ms/frame on the main thread, the ctor below 0.08; rate within the +-1.2 %
  spread of 306 or above it (K129: LOW is near the GPU bound); image correct. H12: CP-only + drain share measured;
  supports H12 at 25 % or more of the samples, refutes it at 5 % or less.
- Refutation: CopyDescriptors unchanged (within 0.05 of 0.63); wrong image (missing or wrong textures would point
  at the held read-only transitions); fault, TDR or removal; gtl probe access denied (then rerun with --set lite).
- Bounds: interactive game session about 7 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU. gtl at most
  60 s, started only after "Start Running" and the world is confirmed on a shot; never across a KMD swap.
