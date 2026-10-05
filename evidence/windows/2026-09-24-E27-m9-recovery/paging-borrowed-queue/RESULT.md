# M425 - Queue admission and retirement from OS-private slots

Source/host integration only. Installed unit A remains M423 KMD0.7.132.1;
this development SYS was not deployed. The original full M9 remains incomplete.

Actual WddmSubmitPagingHardware uses a prebuilt OS-private job slot when the
selected record provides one. The actual queue lock protects claim/link/retire;
a still-owned start is refused without changing its original job. There is no
pool allocation or metadata copy for this queued-format path. Legacy and
zero-byte records explicitly retain allocation during builder migration.

Retirement detaches the job and clears borrowed ownership before recording
completion. It does not touch that storage afterwards. Timeout retains the
pending node; a late real fence permits release. The existing stop-drain block
clears borrowed nodes and frees only legacy heap nodes. This does not prove
hardware quiescence, actual OS cancellation or buffer retention contracts.

Validation:

- The harness extracts the actual job type and queue/fence/watchdog functions,
  plus the actual stop-drain block. The real portable private parser is linked.
  OS locking, pool, timer and GPU completion are mocked.
-590 checks,0 failures: interleaved direct splits, independent equal-VA buffers,
  native records, legacy copy retention, immediate/late fences, timeout retention,
  slot reuse and stop bookkeeping. Allocation-call counts remain unchanged for
  borrowed nodes. This is serialized host interleaving, not a threaded test.
- The completion mock verifies a cleared slot then overwrites it immediately,
  representing OS reuse after completion publication. A copied-source mutation
  reversing release/publication yields91 failures and exit1.
-875868 actual builder/packet checks pass. Builders still emit legacy records;
  this does not establish end-to-end queued construction.
- Full WDK build passes. Development SYS SHA256:
  51ED64099A6180937AD098D0A74A136D06BAE6ED648CFEA8485613D6D9D448D6.
  Still version132, not installed. Installed M423 SYS remains
  E7AF5A02A3DEDA2CAD25E7D6A789FDA3C2406666D537F583D8ACAB14BDA49652.

The first host build failed because its C_ASSERT mock used _Static_assert
without C11 mode. It was corrected to a compile-time typedef assertion; the
initial error log is preserved. No driver defect or hardware rerun was implied.

Remaining: migrate all private builders/budgets/alignment; end-to-end tests;
OS retention/preemption/cancellation/generation acceptance; physical capture
fallback; cache/PFN ownership; cold/power startup and matched performance.
No lab workload, PnP transition, OS/DWM/AC restart or live status poll occurred
in this step. Raw logs are preserved unchanged; no redaction was necessary.
