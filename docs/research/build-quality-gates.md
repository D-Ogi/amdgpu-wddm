# Fast build and candidate gates

Owner direction,2026-09-26: enforce repeatable compiler/ABI/analysis checks without
running a complete conformance or stress suite after each edit.

## Fast profile

Run tools/quality/quick.cmd [workspace] [output-cache] [repository]. Workspace
defaults to BC250_ROOT or the repository parent. MSVC is discovered with vswhere;
outputs stay under the selected workspace. The current KMD build, E34 UMD build
and E35 checked RADV wrapper invoke this profile and propagate failures.

- Compiler contract policy: audit generated commands for48 KMD,108 RADV and24
  UMD C files in the measured configuration. Real compiler controls must accept
  the valid prototype and reject missing declarations and wrong arity.
- ABI:4096 actual Mesa allocation-producer/KMD-parser cases, packing/parser checks.
- Regression:7 cases against the actual pipe_surface_equal implementation.
- Static analysis: actual compile commands for KMD umd_blob.c and UMD Device.cpp.
  C6xxx and C28xxx findings block. This scope is explicit; it is not every file.

Observed runs take a few seconds: approximately3-4seconds with cached analysis,
5-7seconds after relevant changes. Full builds remain incremental where Ninja
supports them. These timings are observations, not a universal time guarantee.
No network, lab, CTS, benchmarks or soak are part of quick.

Analysis cache keys include commands, compiler tools, include search inventory,
source and compiler-reported header hashes. Changed dependencies trigger analysis;
failed runs leave no passing cache. Controls verify header invalidation and a real
C6001 failure. ABI/compiler checks are short enough to run each time.

MSVC /we4013 complements existing fatal argument diagnostics. The missing winsys
prototype and four-argument call were repaired; UMD fixes cover null/null surface
comparison, SAL parameter names and missing D3D11 constants. No global warning
suppression or automatic acceptance baseline was introduced. M537/M538 hold evidence.

## Candidate profile

After quick, run a bounded test of the exact candidate on the lab. The generic
run-smoke.ps1 installs nothing: a separate bounded deployment script selects and
restores the ICD. It checks STOP, finite duration, output oracle, exit status,
unchanged boot and live process module path/hash. Its JSON config names the test,
arguments, expected modules and pass expression; never equate exit0 with correct
pixels or buffers without the oracle.

artifact_gate.py --receipt <receipt.json> --artifact module.dll=<candidate.dll>
--out <qualification.json> rejects failed receipts and changed/untested binaries.
Rebuilding after a test requires a new runtime receipt. Run this check immediately
before staging the artifact. It is a supported gate, not protection against a
human deliberately bypassing the deployment workflow. No CI service is installed.
Kernel identity needs its own device/driver witness; user-process module discovery
does not attest a SYS. M538's receipt covers the tested ICD only, not the new UMD.

## Broader coverage

Full conformance, long stress, Linux performance comparison and release acceptance
remain separate explicit profiles. Expand static-analysis coverage by affected
component and cache per translation unit; current quick intentionally names its
two analyzed files. Hosted ABI/thread/lifetime checks remain separate from the
standalone ICD smoke. Neither quick nor smoke means certification or G0 success.

Build recipes in tools/build and the separate Mesa repository are maintained independently. Integration
accepts BC250_RADV_SOURCE, BC250_RADV_BUILD, BC250_UMD_SOURCE and BC250_UMD_BUILD;
source/build pairs must correspond and builds emit compile_commands.json. Use
those overrides when migrating from the current scratch-tree defaults.

Header tools can assist edits: [clangd include management](https://clangd.llvm.org/guides/include-cleaner).
The build must still reject undeclared calls: [MSVC C4013](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-3-c4013).
