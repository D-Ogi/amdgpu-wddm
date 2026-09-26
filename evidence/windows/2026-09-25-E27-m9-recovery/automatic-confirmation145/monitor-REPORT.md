# Monitor full-WDDM successful-start confirmation

Implements the monitor portion of docs/design/wddm-start-confirmation.md. No deployment or lab/registry/UI actions. Kernel persistence and native transport are owned by the parent/other agent.

Driver.cs adds exact96-byte sequential StartHealthSnapshot and the native Bc250StartHealth call. READ is cached-health-only in the native interface; no subprocess/raw MMIO or staged DLL fallback. Managed validation checks transport and typed status, magic, command21, ABI1 and operation. A confirmed reply must carry a nonzero full-WDDM identity. CONFIRM must return the exact expected generation/epoch and all required/confirmed flags. An unavailable/old DLL or ABI fails closed.

StartConfirmationPolicy.cs is pure and uses monotonic elapsed milliseconds. Full confirmation requires at least60000ms of observations of one generation/epoch with FULL|READY|VISIBLE, Completed>0, completion age<=15000ms and strictly increasing completed count at every subsequent sample. Poll gaps>15000ms, counter/ready-age regression, missing/unhealthy samples and identity changes restart observation. ReadyAgeMs must also be>=60000 at confirmation. A repeatedly queried old completion cannot qualify. A quiet desktop can defer confirmation; this is intended.

KmdProvider invokes one READ per five-second poll while a registry start is pending. It never uses stage50 as a full-WDDM success condition. Full confirmation goes through the typed expected-identity operation, not KmdRegistry.Confirm. Only a validated READ explicitly reporting FULLfalse admits the existing display-only stage61/60s policy. Missing/unreadable registry state resets observation. The explicit human KmdRegistry.Confirm implementation, including its checked RegFlushKey return, is unchanged.

A failed typed confirm remains pending in the monitor even if registry write-before-flush left a cached zero: this case must not become 'nothing to confirm'. Retry starts a fresh observed interval. Successful typed confirmation or a validated current confirmed-health reply clears that retry. Registry zero by itself never emits the full-WDDM confirmation success log.

Validation:
- Actual policy + actual Driver reply validation + extracted actual provider methods:120 checks. Boundaries for transport, clock and registry are fakes; no P/Invoke or host registry/UI is exercised. Cases include60s progress, no progress, same registry counter with new generation, epoch/gap/regression/missing/stale samples, required flags, ready age, old DLL/ABI, positive DDO policy, typed failures, epoch changed immediately before confirm, and cached-zero retry after failed confirm.
- StaleCounterPolicy.cs negative mutation permits unchanged counters; it fails 'same completed surface cannot confirm' in stale-counter-negative.log.
- Six Python stage/ABI checks pass. ABI test compares field order/types against the shared C header, not a second manually maintained layout.
- Existing inventory34 checks pass.
- build.log: complete .NET Framework4.8 monitor build succeeds with native DLL from scratch/build/health145-client/bc250control.dll. Executable was compiled but never started on this PC.

Build output: scratch/build/monitor-start-health/. Source snapshot and manifest: source/sha256.json. Files changed: src/Driver.cs, src/KmdProvider.cs, new src/StartConfirmationPolicy.cs, build.ps1, new test-start-confirmation.ps1, test_start_health_abi.py, test/StartConfirmationTest.cs and test/generate-start-confirmation-test.py. Production edits frozen; parent owns integrated candidate build/deployment and evidence/status.

Acceptance still required: a genuine new full-WDDM device start, monitor-observed60s progress, exact generation/epoch confirmation with successful durable kernel flush, and retained live OS/DWM/display checks. The monitor policy proves neither pixel correctness nor M9 completion.
