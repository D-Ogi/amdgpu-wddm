# Release trains

How a batch of finished work reaches the lab and then a tester. The owner set the method on 2026-10-05 and
changed one rule of it on 2026-10-06. Before that, each feature asked the lab for its own A/B, and the lab
became the bottleneck. A train replaces that with one integration branch and one lab validation per batch.

## The rule

1. **Host gates first.** A feature must pass the gates of its own workflow: build, unit tests, ABI gate,
   review. A feature that fails one of them waits for the next train. The lab never sees it.
2. **One branch, one build.** Every feature that passes goes into one integration branch and one release build.
3. **One lab validation.** The lab validates the train. It does not validate each feature alone.
4. **A switch per risky feature, for bisection only.** Each risky feature keeps a runtime switch, a registry
   value or a package choice, so a failure on the lab is isolated by bisection without a rebuild.
5. **A finished feature rides ON by default** (owner, 2026-10-06). The switch exists to bisect a failure, not
   to hold the feature back. Only operator settings and diagnostics stay absent by default: those are values a
   person sets, not features.
6. **Pass means main and the release on the same day.** When the train passes on the lab, it goes to main and
   to the tester release together.

Rule 5 is the 2026-10-06 change, and it is worth stating why it matters. The first b20 build shipped every addition
default-off. That is a per-feature A/B in disguise: nothing is exercised, so the single lab validation proves
nothing about the new code.

## What a wagon record holds

Each wagon of a train names five things, and a train plan is a table of them:

- The feature and the criterion it serves.
- Its source branch and the exact commit.
- The artifacts it produced, each with its SHA-256.
- Its host gate result.
- Its switch, and what the switch restores.

The artifact hashes are the reason a train can be bisected at all. Two builds of the same commit produce the
same binary (`/Brepro` and `/PDBALTPATH`, `docs/design/reproducible-builds.md`), so a hash names a revision
rather than a build.

## The trains so far

### b18 and b18r1, release 0.7.207.100-tester.12

The first train. Wagons: independent flip increment 1 (KMD 0.7.206, switch `EnableScanoutAdmit`), the idle
500 MHz DPM state (switch `DpmIdleMHz=0`), the GPU H.264 encoder MFT (switch: the transform is not registered),
the overlay's active-3D-application row, the four upstream engine merges (switch: the deployed engine DLL), and
the work ledger with its release gate.

b18 became b18r1 after a fix, and b18r1 passed its lab validation on 2026-10-05. The lab ran KMD 0.7.207.1 with
DPM on and the idle state active. One wagon of the plan did not ride: the Mesa merge faulted the GPU from a
WebView2 process on the D3D11 route, so it waited for b19 with a fix.

### b19, release 0.7.208.100-tester.13

Installed on the lab on 2026-10-06 and validated there. Ten wagons, among them the KMD log lines and the DPM
closed reason, the fixed Mesa ICD pair, the D3D11 shell NV12 encoder input with a DDI error-status gate, the
D3D12 shell wagon (heap-import unlock, direct recording entry), the two desktop UMD wagons that fixed the
surface-setup leak, the 32-bit artifact wagon, and the interim fail-fast form of the shared-resource fix.

Three decisions of b19 show the method working rather than the features:

- Independent flip increment 2 left the train because its parts were not ready. The train did not wait.
- Same-context wait elision left the train because its own acceptance rule refused it. The code stayed as an
  instrument, default off, and no release depends on it.
- The real shared-resource fix failed its lab pre-validation 4 of 12, so b19 shipped the fail-fast form and the
  real fix went to the next train.

A fourth decision changed this method: a respin of b19 was planned and then dropped, because three
separate kernel-driver installs on the lab are three validations. The work moved to one b20 KMD instead.

### b20, in flight

Five kernel-driver branches each claimed a version between 0.7.208.1 and 0.7.211. b20 builds one kernel driver
from one stack and gives it one new version, so no claimed number is reused. That is the general rule for a
train whose wagons all touch one component.

## Where the live plan is

A train's working plan, its wagon table and its lab record live in the workspace while the train is in flight,
because they name lab install paths and candidate packages. This document holds the method and the record of
each train after it closes. The deployed state is always the workspace `STATE.md`, never a document here.
