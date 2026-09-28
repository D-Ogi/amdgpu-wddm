# Native D3D12 queue contract probe

Build with `build.ps1 -Kits <workspace>/toolchain/nuget -Out <workspace>/scratch/build/d3d12queue`.
Run `amdgpu_wddm_d3d12_queue.exe --warp` for the software positive control or `--lab` for the BC-250 adapter (PCI1002:13fe). The program explicitly loads System32/d3d12.dll. It does not load the vkd3d engine directly or change driver registration.

The sequence creates two direct queues and two fences. Queue A signals done=1; CPU observes completion. Queue B waits on gate=1 and then signals done=2. During a100ms observation, done must stay1 and its event must time out. CPU signals gate=1; done must then reach2 within5s. The device must remain healthy. Any failed assertion produces a nonzero exit. The gate is released even after a failed blocked-state assertion.

Run under the existing bounded-child Job supervisor: the5s waits do not bound driver calls or COM teardown. All lab trials remain at most180s and require exact baseline/rollback and sibling-module admission from the surrounding runner. This client is not permission to deploy an unfinished DDI shell. Output is flushed after each operation so a stopped attempt retains its last boundary.

Host validation,2026-09-28: /W4 /WX build, help and malformed CLI controls pass. WARP completes the sequence, done stays1 during WAIT_TIMEOUT and becomes2 after CPU release; process exits0 and the20s Job supervisor verifies an empty Job. Raw receipt: scratch/m15/queue-client001. No native lab run yet.

This measures API ordering for a no-command-list sequence. It does not establish which native DDI callbacks or KMD contexts implement the ordering. The T0 logging shell must identify those, including the association with hRTCommandQueue. Copy/compute, GPU cross-queue dependencies, residency and FL12_1 require subsequent tests.

## Temporary DX12 registration

`registration.ps1` plans and applies a reversible fourth `UserModeDriverName`
REG_MULTI_SZ entry. It requires an exact three-entry baseline, preserves its
ordering and values, and refuses foreign modifications during restoration.
Persist the plan before installation; hold the caller's mutation lock throughout
all phases. A failed install can have written the value and still needs restoration.
Confirm every writer's process tree is closed before restoring or verifying.

Run `powershell -NoProfile -File registration-test.ps1` before packaging a trial.
The mock-key tests cover JSON plan round-trip, normal and repeated restoration,
post-write failure, foreign mutation, wrong registry kind and tampered plans.
They do not establish that dxgkrnl refreshes its effective DX12 driver name.
The trial must query that name through KMT after installation and after restoration;
a registry readback alone is insufficient. No restart or persistent deployment is
implicit in this helper.
