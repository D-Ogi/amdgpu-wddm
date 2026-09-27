# M684: map lifetime audit candidate

PROVENANCE: Mesa fork (MIT), source 150631a8584a5c8c8ae676b09820561d6317438d, parent a63dade027d6e16c626ac2daab4fc2ea3dcaa690.

Candidate UMD SHA256 `2A772DD45B71EAAD450A203D5ADA3CD158F178AF59DCCDC09FEE5F09DE473F64`, 15178752 bytes. Complete UMD target
build passes; no lab deployment or performance measurement.

The opt-in BC250_HOST_AUDIT path assigns process/DLL-local 64-bit identities at
logical resource and resource-object allocation. Map begin records the requested
resource/object; result records success from the actual returned pointer and the
actual resource/object after DISCARD, transient or staging selection. End uses
only the transfer's map ID, including screen-only unmap without a context. These
are Gallium transfer lifetimes, not low-level Vulkan/BO mapping lifetimes.

The host control compiles the exact ID and event helpers extracted from this
revision against Mesa's atomic header, with stub resource metadata and clock.
It checks disabled auditing, object replacement under the same resource pointer,
staging, NULL-result and NULL-transfer failures, IDs across 2^32, 64-bit layer
stride and context-free end. It is not a map-path or concurrency integration test.
The checked extraction matches the committed helpers byte-for-byte.

The trace has 4 requests, 2 successful results, 2 failures and 2 ends. The checker
recognizes one object replacement and one staging success. Nine malformed
variants are rejected, including missing/duplicate events and zero identities;
explicit allow-live mode reports the remaining map. Rejections also run under
Python -O, since production validation uses explicit checks rather than assert.
Run the retained fixture from the workspace root:

```powershell
python bc250-win/experiments/E34-native-d3d-zink/hosted-runtime/test-map-lifetimes.py bc250-win/evidence/windows/2026-09-27-E34-map-lifetime-build/control.events.log
```

An initial compiler format warning for uintptr_t layer_stride was corrected to
64-bit output before the final build. The harness initially lacked Mesa's include
path; the retained successful recipe contains it. Earlier logs remain in private
scratch/g0-hosted/map-lifetime003. The DLL stays there, outside the repository.

Limits: successful events do not count subsequent CPU stores. Missing entire
maps cannot be detected from this event stream alone. An explicit interval boundary,
reconciliation against aggregate requests, uploader identity correlation, capture
brackets and deliberate-copy controls remain required before G0 no-copy acceptance.
No new lab conclusion is drawn from this host build/control.
