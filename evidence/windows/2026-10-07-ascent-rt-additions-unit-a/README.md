# The Ascent: the refused ray tracing addition on unit A (2026-10-07)

Date: 2026-10-07 (lab capture) and 2026-10-08 (harness). `lab-capture.txt` is from unit A: Windows 11 Pro,
D3D12Core.dll 10.0.22621.5415 in System32 (fact M758), the shell DLL 1AC48EBC and the engine DLL 348117F1. The
harness ran on the development PC with an NVIDIA GeForce RTX 4090. Every program ran without a window and stopped
by itself.

## Question

Fact M838 says that engine-ddi accepts the state objects of Unreal Engine 4.26 with additions. That result is from
the runtime 10.0.26100 on the development PC. On unit A, the `ue426-add` case of `d3d12ddicap` gets 8007000e from
`AddToStateObject`. What does the driver refuse, and why?

## Method

1. The lab operator ran `d3d12ddicap.exe` (SHA-256 B05CCE84, first line of `lab-capture.txt`) on unit A with
   `--hardware` and the cases `ue426-add-miss`, `ue426-add-chs`, `ue426-ahs-link` and `ue426-add`. The driver log
   went to a file (`AMDGPU_WDDM_LOG=file:<path>`). The engine wrote its pipeline log
   (`AMDGPU_WDDM_VKD3D_PSO_LOG=1`). `lab-capture.txt` holds the client output, the state object lines of the driver
   log and the pipeline log.
2. The source of the engine (vkd3d-proton fork 4e9a98e9) was read at the refusal.
3. The fix in `driver/umd/d3d12/engine-ddi/state-objects.cpp` (this commit series) was tested with the engine-ddi
   harness, round trip 9, new case 7d. Case 7d sends the UE 4.26 link and addition in unit A's form: no state object
   configuration. Three runs with the fix: the pinned engine, the pinned engine with deferred replay, and the lab
   engine 348117F1. One control run: the same harness source with the `state-objects.cpp` of the parent commit, on
   the lab engine.

## Files

| File | Content |
|---|---|
| `lab-capture.txt` | Unit A: the client results, the state object lines of the driver log, the pipeline log of the engine |
| `harness-raytracing.txt` | Harness round trip 9: SHA-256 of the harnesses and engines, the UE lines of three fixed runs and one control run |
| `sha256.txt` | SHA-256 of every file above |

## Result

1. The runtime on unit A gives each collection its state object configuration (subobject type 0x0). It gives a
   link of collections no state object configuration (types `0x6 0x6 0x6 0xa 0x100000`). It also gives an addition
   none (types `0x6 0xa 0x100000` or `0x6 0x6 0xa 0x100000`). The client put a configuration that allows additions
   into each link and each addition. On the development PC the runtime 10.0.26100 gives it to the driver (fact
   M838).
2. Each of the three additions gets `hr 80070057 reported as 8007000e` on the driver log line of
   `AddToStateObject`. The link of `ue426-ahs-link` (a hit group with an any hit shader) gets 00000000 with all
   three identifiers, so the any hit shader is not the cause. The device removed reason stays 00000000.
3. The engine refuses the addition before it compiles a pipeline: the pipeline log has no `rt` row after the
   addition. `raytracing_pipeline.c:2888-2907` returns E_INVALIDARG unless the flags of the parent contain
   `ALLOW_STATE_OBJECT_ADDITIONS` and the addition has a state object configuration with that flag. The parse
   (`:963-984`) takes the flags only from a state object configuration subobject. engine-ddi passed none, so the
   base link was made without the flag.
4. The fix. engine-ddi adds a state object configuration with `ALLOW_STATE_OBJECT_ADDITIONS` to the description
   for the engine in two cases. The first is an addition with no configuration. The second is a ray tracing
   pipeline with no configuration, no library and no hit group, whose imports all allow additions. The D3D12 ray
   tracing specification requires the flag on both the original and the addition (`Raytracing.md` lines 3785 and
   3380).
5. Harness with the fix: 572 checks pass on the pinned engine, also with deferred replay, and 573 on the lab engine
   348117F1. In case 7d the engine gets one configuration with flags 4 for the link and for each addition. The
   addition gives S_OK, grows from the link's engine object, and keeps the ray generation identifier of the link. A
   second addition onto the grown object also gives S_OK. In the control part of 7d, collections without the flag
   give a link with no configuration, and an addition onto it is refused with one refusal line.
6. Control run with the code of the parent commit: 570 pass and 3 fail. The 3 failures are the three unit A form
   checks of 7d: the link gets 0 configurations, the addition gets 8007000e, and the second addition gets
   80004005. This is the result of unit A.

## Limits

- The fix is not measured on unit A. The RADV path of an addition (a pipeline library and a link of the parent) is
  not shown on unit A.
- A pipeline with its own library or hit group and no configuration does not get one. That the unit A runtime
  forwards the configuration of such a pipeline is an INFERENCE from the collections.
- Case 7d checks identifiers only. The dispatch through a grown object is checked in case 7c, in the form of the
  development PC.
- The game itself has not run with the fix.
