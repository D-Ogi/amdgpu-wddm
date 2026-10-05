# Shared inherited refresh: source fix for the 136 -> 139 regression

No lab action by this sub-agent. Source frozen for parent candidate 143.

The VidPn target/monitor mode changed from nominal 60 Hz to measured
154000000/2568800 Hz (59.950171286), but DescribeAllocation still returned
60000/1000. This violates the intended equality of the current single inherited
primary mode and its allocation description. The local DDI contract states
RefreshRate is the rate the primary surface was created with:
ref/ddi-display/d3dkmddi.md:32170. E26/M147 already measured how a mismatching
allocation refresh produces PRESENT_MODE_CHANGED. The root agent additionally
captured 41 current142 SetDisplayMode events, all C01E0005.

Timing itself is supported: current VideoStandard OTHER, progressive ordering and
non-Miracast divider zero are unchanged from136 and agree with local MS docs
(ref/ddi-display/d3dkmdt.md:1610-1652). Sixteen own-unit139 snapshots and QDC agree
on active1920x1200,total2080x1235,pixel154MHz, and independent OTG frame counting
measured59.955Hz against decoded59.950Hz (scratch/m13/observe139-analysis.json).

Changes:
- display.c: DisplayPrepareInheritedTiming publishes a complete signal tuple once
  at startup. FillSignalInfo copies that tuple rather than re-reading MMIO.
- bc250kmd.h (peer edit): immutable cached signal, valid flag, prepare prototype.
- pnp.c: invalidate on Start/Stop; prepare after successful WddmStart and before
  Started/adapter admission. Failed timing preparation unwinds the started device.
  Display-only preserves unspecified timing without MMIO.
- wddm.c, DescribeAllocation only: report the same cached VSync rational; log it.
- Existing actual-source host timing test now extracts both functions and checks
  exact agreement, immutable reuse, no reads in either query, and invalidation.

Checks:
- host-positive.log:205 checks,0 failures.
- host-old60-negative.log: same205 checks,3 expected failures when only the
  DescribeAllocation frequency is mutated back to60000/1000.
- own-unit-replay.log: captured139 raw register tuple decodes successfully through
  new prepare/cache/Fill path,22 reads,154000000/2568800 Hz.
- compile.ps1: actual display.c,wddm.c,pnp.c compiled /kernel /W4 /WX against
  WDK10.0.26100 (objects under scratch/build/display-refresh143/wdk).

Parent owns frozen production package, versions, full link/build, deployment,
physical visibility confirmation and fresh ETW proving PRESENT_MODE_CHANGED gone.
No new shared defect/fact/state entry was made by this agent.
