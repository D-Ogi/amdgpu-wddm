Report of the first full run, 2026-09-21; fixes of 2026-09-22 are listed at the end.

# SDMA bring-up under fault injection

Host-side matrix against `driver/shim/bc250_sdma.c`, run with an instrumented allocator (live-object
table, poison on free, fault at the Nth call) and a register file that can make an engine answer with
a write pointer the driver did not choose. No register sequence was changed and none was compared
with a trace here; `run_gfx.ps1` still says EXACT MATCH over 354 + 35 writes.

Deliverables:

- `driver\shim\test\sdma_faults.c` - the suite, self-contained: its own allocator
  and register backend, so nothing it does can reach the replay's `backend_mem.c`.
- `driver\shim\test\run_sdma_faults.ps1` - build and run, output under
  `<BC250_ROOT>\scratch\build\sdma-faults`, exit code non-zero on a failed expectation.
- `driver\shim\test\README.md` - new file (there was none); what the suite is and
  how an expected failure is written.

State today: **161 checks, 0 failures, 15 expected failures over 7 confirmed defects, exit 0.**

## The matrix

| # | Case | Verdict |
|---|------|---------|
| 1a | ring/write-back/fence allocation fails, each in turn (calls 0, 1, 2, 3) | **PASS** - error returned, every earlier allocation released exactly once, no double free, no stray free, no write into freed memory, and the failed setup writes no register at all |
| 1b | the same for engine 0 and engine 1 separately | **PASS** |
| 1c | retry after a failure, and teardown twice after it | **PASS** |
| 2a | write-back page returns `cpu == NULL` | **DEFECT D-01** (accepted), **DEFECT D-02** (leaked) |
| 2b | ring buffer returns `cpu == NULL`, engine 0 and engine 1 | refusal **PASS**, **DEFECT D-02** (the refused object is unfreeable) |
| 2c | fence page returns `cpu == NULL` | refusal **PASS**, **DEFECT D-03** (leaked, and a retry leaks a second page) |
| 3 | `slot = ring->me & 1`, a caller with `me = 2` | **DEFECT D-07** |
| 4a | preserved wptr all-ones (both halves, low half, high half) | **PASS** - `BC250_EINVAL`, ring not enabled, no write outside the confirmed set |
| 4b | preserved wptr not dword aligned / not a whole 16-dword submission / one dword | **PASS** - `BC250_EINVAL` |
| 4c | preserved wptr past the end of the ring | **PASS** - adopted on purpose; `buf_mask` wraps it (0x2040 -> dword 0x10 of 0x800) |
| 4d | preserved wptr wildly large (0x7FFF_FFFF_FFC0) | **PASS** - adopted, programmed back, published into the shadow before `RB_ENABLE`, indexing stays in the ring |
| 5 | engine 1 refuses after engine 0 has started | state is **unambiguous but not safe**: engine 0 left running (upstream same, see below); `hw_fini` correctly reports `BC250_EBUSY`; a second setup is **DEFECT D-04** |
| 6a | `start` and `hw_fini` after a failed setup | **DEFECT D-05** |
| 6b | unwind exactly once, fini twice, fini after a failed setup | **PASS** |
| 6c | the rings after a teardown | **DEFECT D-06** (use-after-write into a freed ring buffer and a freed write-back page) |

## Defects

### D-01 `bc250_sdma_setup()` accepts a write-back page it cannot address

`driver/shim/bc250_sdma.c:633-637` and `:669-671`. The ring buffer's `cpu == NULL` is refused eight
lines below (`:653-656`); the write-back page's is not, and `ring->wptr_cpu_addr` is simply left NULL.

Scenario: an allocator honours the contract in `driver/shim/include/bc250_shim.h:31-38` ("`cpu` may be
NULL if the owner cannot map the memory; the shim then refuses to build anything that needs CPU
access rather than guessing") and hands back the write-back page unmapped. `bc250_sdma_setup()`
returns 0. `bc250_sdma_start()` then programs `SDMA0_GFX_RB_WPTR_POLL_ADDR_LO/HI` with the page's MC
address and sets `F32_POLL_ENABLE` (`:396-407`): the engine is told to poll a shadow the driver can
never write. `amdgpu_ring_commit()` skips the shadow too (`bc250_ring.c:115-116`), so the doorbell is
the only thing that carries the write pointer.

Reachability: neither shipped backend produces it - `backend_mem.c:299-301` and `gpumem.c:293-294`,
`:317-318` both return `-12` when the mapping fails. So this is a contract-level defect today, one
backend away from being real.

```diff
--- a/driver/shim/bc250_sdma.c
+++ b/driver/shim/bc250_sdma.c
@@ -633,6 +633,13 @@ int bc250_sdma_setup(struct amdgpu_device *adev)
 	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_SDMA_WB_BYTES, AMDGPU_GPU_PAGE_SIZE,
 				 &adev->sdma.wb_mem);
 	if (r)
 		goto fail;
+	/* The write pointer the engine polls lives in this page. Without a CPU mapping there is
+	 * nothing to publish it into, and the ring's own cpu == NULL is refused below for exactly
+	 * that reason (bc250_shim.h: the shim refuses rather than guesses). */
+	if (adev->sdma.wb_mem.cpu == NULL) {
+		r = BC250_EINVAL;
+		goto fail;
+	}
```

The `if (adev->sdma.wb_mem.cpu != NULL)` at `:669` then becomes always true; leaving it costs nothing.

Upstream: no counterpart. `amdgpu_bo_create_reserved()` fails the whole allocation when the kernel
map fails (`ref/linux-src/.../amdgpu_object.c:302-308`), so amdgpu has no "allocated but unmapped"
state to accept.

### D-02 an allocation whose `cpu` is NULL can never be given back

`driver/shim/test/backend_mem.c:320-321` and `driver/kmd/gpumem.c:378`, reached from
`bc250_sdma.c:653-656` and `:843-844` (and `bc250_gfx.c:1710-1711`, `:1725-1728`, out of scope here).

`bc250_shim_mem_free()` returns at once on `m->cpu == NULL` and otherwise looks the object up by that
pointer. So every "allocated, but I cannot address it" refusal in the shim leaks the object it just
got: the shim holds `mc` and `size` and has no way to say so. The suite shows one live allocation
after each of cases 2a and 2b.

Two ways out; the lead picks.

(a) Make free able to release it - smallest change that keeps the contract as written:

```diff
--- a/driver/kmd/gpumem.c
+++ b/driver/kmd/gpumem.c
@@ -375,11 +375,12 @@ void bc250_shim_mem_free(struct amdgpu_device* adev, struct bc250_mem* m)
-    if (mem == NULL || m == NULL || m->cpu == NULL) return;
+    // An allocation the caller could not map still exists here: look it up by its MC address instead.
+    if (mem == NULL || m == NULL || (m->cpu == NULL && m->size == 0)) return;
     for (i = 0; i < BC250_GPUMEM_MAX; i++)
     {
         BC250_GPUMEM_ENTRY* entry = &mem->Entries[i];
-        if (!entry->Used || entry->Cpu != m->cpu || entry->Retired) continue;
+        if (!entry->Used || entry->Retired) continue;
+        if (m->cpu != NULL ? entry->Cpu != m->cpu : entry->Mc != m->mc) continue;
```

plus the same two lines in `backend_mem.c:315-332`, and one sentence in `bc250_shim.h:85-86`.

(b) Tighten the contract instead: `bc250_shim_mem_alloc()` must never return 0 with `cpu == NULL`
(both backends already behave that way), `bc250_shim.h:31-38` says so, and the four `cpu == NULL`
rejections in the shim stay as cheap assertions. That deletes D-01 and D-03 along with D-02, at the
price of a documented capability nobody uses.

Upstream: no counterpart, same reason as D-01. `amdgpu_bo_free_kernel()` takes the BO, not a CPU
pointer.

### D-03 `bc250_sdma_fence_page_alloc()` loses the page it just got

`driver/shim/bc250_sdma.c:843-844`.

```c
	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, AMDGPU_GPU_PAGE_SIZE, AMDGPU_GPU_PAGE_SIZE,
				 &adev->sdma.fence_mem);
	if (r)
		return r;
	if (adev->sdma.fence_mem.cpu == NULL)
		return BC250_EINVAL;            /* the object stays in adev->sdma.fence_mem, unfreed */
```

Two consequences, both shown by the suite. The page is leaked (D-02's mechanism). And the idempotence
test one line above, `:836-837`, keys on `fence_mem.cpu`, which is NULL - so the next call allocates a
second page straight over the descriptor of the first. `driver/kmd/gfx.c:384-388` makes exactly that
call on every fence IOCTL while `SdmaFencePage` is FALSE, which is the state a refusal leaves behind:
one leaked page per submission attempt.

```diff
--- a/driver/shim/bc250_sdma.c
+++ b/driver/shim/bc250_sdma.c
@@ -841,8 +841,13 @@ int bc250_sdma_fence_page_alloc(struct amdgpu_device *adev)
 	if (r)
 		return r;
-	if (adev->sdma.fence_mem.cpu == NULL)
+	if (adev->sdma.fence_mem.cpu == NULL) {
+		/* Give it back and leave the descriptor empty: the idempotence test above keys on
+		 * fence_mem.cpu, so a descriptor left half full would have the next call allocate a
+		 * second page over this one. (The free itself needs D-02.) */
+		bc250_shim_mem_free(adev, &adev->sdma.fence_mem);
+		adev->sdma.fence_mem.mc = 0;
+		adev->sdma.fence_mem.size = 0;
 		return BC250_EINVAL;
+	}
 	return 0;
```

Without D-02 the two explicit stores still stop the second leak; only the first page is lost.

Upstream: no counterpart. `sdma_v5_0_ring_test_ring()` takes its scratch dword from
`amdgpu_device_wb_get()` (`reference/sdma_v5_0.c:1023-1027`) and frees it on every error path.

### D-04 a second `bc250_sdma_setup()` leaks the first one's three allocations

`driver/shim/bc250_sdma.c:627-674`. Nothing checks whether `adev->sdma` already holds allocations;
`bc250_shim_mem_alloc()` overwrites `wb_mem` and both `ring_mem` descriptors, and the previous three
pages are lost - the suite counts 6 live objects after the second setup, 3 after the teardown that
follows. The asymmetry is worth naming: `bc250_sdma_fence_page_alloc()` is idempotent and says so in
the header, `bc250_sdma_setup()` is neither and does not.

Scenario: case 5 - `bc250_sdma_start()` fails on engine 1, the caller decides to try the whole
bring-up again without an undo.

```diff
--- a/driver/shim/bc250_sdma.c
+++ b/driver/shim/bc250_sdma.c
@@ -624,6 +624,12 @@ int bc250_sdma_setup(struct amdgpu_device *adev)
 	if (adev == NULL)
 		return BC250_EINVAL;
+	/* Not idempotent, unlike bc250_sdma_fence_page_alloc(): allocating over the descriptors
+	 * would lose three pages, and the engines may still be fetching from the rings they name.
+	 * A caller that wants a second bring-up runs bc250_sdma_teardown() first, which is what
+	 * driver/kmd/gfx.c's Fini() does. */
+	if (adev->sdma.wb_mem.size != 0)
+		return BC250_EINVAL;
```

No existing suite calls setup twice without a teardown or a `memset(adev)` in between
(`replay_gfx.c:1602`, `:2078`), so the guard changes nothing that runs today.

Upstream: `sdma_v5_0_sw_init()` has no guard either, but the IP-block core calls it once per device
and calls `sw_fini` on failure. Ours is called by `driver/kmd/gfx.c:137`, which also checks. Call it
upstream-same, worth fixing because the shim's entry points are the miniport's API surface.

### D-05 `bc250_sdma_start()` does not check that the setup finished

`driver/shim/bc250_sdma.c:516-535`. `adev->sdma.num_instances` is set at `:627`, *before* the three
allocations, and `bc250_sdma_teardown()` does not put it back. So after a failed setup the device says
"two engines" while both rings have `funcs == NULL`, `ring_size == 0` and `gpu_addr == 0`.

What the suite measured: `bc250_sdma_start()` returns **0** and enables both engines -
`SDMA0_GFX_RB_BASE = 0`, `RB_CNTL = 0x00001001` (`RB_SIZE` 0, `RB_ENABLE` 1), `IB_ENABLE` 1. Two
engines fetching from MC address 0 with a 4-byte ring.

The worse arm is not exercised by the suite on purpose: on a re-init the engine keeps its write
pointer (facts M59/M60), `hw_wptr != 0` at `:352` is taken, and `:377` dereferences
`ring->funcs->align_mask` with `funcs == NULL`. In the miniport that is a bugcheck.

Reachability: `driver/kmd/gfx.c:137-138` checks the setup's result and tears the GFX side down, so no
caller in the tree reaches this today. It is one caller away, and the guard is four lines.

```diff
--- a/driver/shim/bc250_sdma.c
+++ b/driver/shim/bc250_sdma.c
@@ -518,6 +518,13 @@ int bc250_sdma_start(struct amdgpu_device *adev)
 	if (adev == NULL)
 		return BC250_EINVAL;
+	/* The rings bc250_sdma_setup() filled in, or nothing at all: num_instances is set before its
+	 * allocations and the teardown does not put it back, so a setup that failed leaves two
+	 * engines' worth of zeroed rings behind. Enabling those would point the engines at MC 0, and
+	 * the adoption arm below would dereference ring->funcs. */
+	for (i = 0; i < adev->sdma.num_instances; i++)
+		if (adev->sdma.instance[i].ring.funcs == NULL ||
+		    adev->sdma.instance[i].ring.ring_size == 0)
+			return BC250_EINVAL;
```

`i` is already declared in the function. Alternatively (or as well) have `bc250_sdma_teardown()` set
`adev->sdma.num_instances = 0`; that one line would also make this state harmless, but it changes what
`bc250_sdma_hw_fini()` walks after a teardown, so the guard above is the smaller change.

Upstream: same absence of an internal guard, same protection by the caller
(`amdgpu_device_ip_init()` stops at the first failing `sw_init`).

### D-06 `bc250_sdma_teardown()` leaves the rings looking usable

`driver/shim/bc250_sdma.c:683-694`. The three `bc250_mem` descriptors are zeroed by the free, but
`ring->ring`, `ring->gpu_addr` and `ring->wptr_cpu_addr` are separate copies and keep pointing at the
pages that have just gone back.

Scenario: a caller that runs `bc250_sdma_ring_test()` after the teardown. Every guard in the ring test
passes - `ring`, `ring->adev`, `ring->funcs` and `ring->ring` are all non-NULL (`:881-882`) - and it
writes a five-dword packet plus eleven NOPs into the freed ring buffer, then `amdgpu_ring_commit()`
writes the write pointer into the freed write-back page (`bc250_ring.c:115-116`). The suite's poison
check counts **2** freed allocations written into, and the call returns `BC250_ETIME` rather than a
refusal.

Reachability: the documented order is fence page first, teardown after (`bc250_sdma.h:100-103`), and
`driver/kmd/gfx.c:150-154` does it that way; the fence IOCTL also refuses unless `gfx->SetUp`
(`gfx.c:374`). So this needs a caller's mistake - which is exactly what `bc250_gfx.c:1702-1704` says
the CP rings must be proof against: "leaves `ring->ring` NULL, so that a ring whose allocation did not
finish cannot be written to". The SDMA teardown does not keep that rule.

```diff
--- a/driver/shim/bc250_sdma.c
+++ b/driver/shim/bc250_sdma.c
@@ -684,12 +684,20 @@ void bc250_sdma_teardown(struct amdgpu_device *adev)
 {
+	struct amdgpu_ring *ring;
 	int i;
 
 	if (adev == NULL)
 		return;
 
-	for (i = 0; i < AMDGPU_MAX_SDMA_INSTANCES; i++)
-		bc250_shim_mem_free(adev, &adev->sdma.instance[i].ring.ring_mem);
+	for (i = 0; i < AMDGPU_MAX_SDMA_INSTANCES; i++) {
+		ring = &adev->sdma.instance[i].ring;
+		bc250_shim_mem_free(adev, &ring->ring_mem);
+		/* The rule bc250_ring_alloc_mem() states for the CP rings (bc250_gfx.c:1702-1704):
+		 * a ring without a buffer is unusable by construction, not by the caller
+		 * remembering. Upstream does the same - amdgpu_bo_free_kernel() nulls ring->ring
+		 * and zeroes ring->gpu_addr (amdgpu_object.c:528-532). */
+		ring->ring = NULL;
+		ring->gpu_addr = 0;
+		ring->wptr_cpu_addr = NULL;
+	}
 
 	bc250_shim_mem_free(adev, &adev->sdma.wb_mem);
 }
```

Upstream: **differs, and gets it right.** `amdgpu_ring_fini()` frees the buffer through
`amdgpu_bo_free_kernel(&ring->ring_obj, &ring->gpu_addr, (void **)&ring->ring)`
(`amdgpu_ring.c:416-418`), which sets `*cpu_addr = NULL` and `*gpu_addr = 0`
(`amdgpu_object.c:528-532`). The same holds for `bc250_gfx_teardown()`, which is out of scope here but
has the identical gap.

### D-07 `bc250_sdma_ring_test()` folds an out-of-range engine number onto a valid slot

`driver/shim/bc250_sdma.c:890`: `slot = ring->me & 0x1u;`.

A ring with `me = 2` does not get a refusal: it gets engine 0's scratch slot. The suite ran a ring
test on such a ring and watched slot 0 go from its sentinel to `0xCAFEDEAD`, with the emitted
`WRITE_LINEAR` carrying `bc250_sdma_fence_addr(adev, 0)` - so a third "engine" would seed, submit
against and poll the slot engine 0 is using, and the two would overwrite each other's answers. The
call returned `BC250_ETIME` in the suite (no stub executes SDMA packets there); on hardware it would
return 0 for the wrong reason as soon as engine 0's own test happened to run.

```diff
--- a/driver/shim/bc250_sdma.c
+++ b/driver/shim/bc250_sdma.c
@@ -887,7 +887,10 @@ int bc250_sdma_ring_test(struct amdgpu_ring *ring)
 	if (adev->sdma.fence_mem.cpu == NULL)
 		return BC250_EINVAL;            /* bc250_sdma_fence_page_alloc() was not called */
 
-	slot = ring->me & 0x1u;                 /* one scratch slot per engine */
+	/* One scratch slot per engine, and no folding: an engine number this driver does not have
+	 * is a caller's mistake, and masking it would have that caller seed, submit against and
+	 * poll a slot another engine is using. */
+	if (ring->me >= AMDGPU_MAX_SDMA_INSTANCES)
+		return BC250_EINVAL;
+	slot = ring->me;
```

Upstream: **differs, no equivalent weakness.** `sdma_v5_0_ring_test_ring()` takes a fresh write-back
slot per call from `amdgpu_device_wb_get()` and never indexes anything by `ring->me`.

Related, and **not** a defect of ours: `bc250_sdma_reg_offset()` folds `instance >= 2` onto instance
0's register window (`:89-94`), because it tests `if (instance == 1)`. Upstream's
`sdma_v5_0_get_reg_offset()` (`reference/sdma_v5_0.c:218`) is written exactly the same way. Left
alone; the transcription rule wins.

## Findings that are not defects

- **A refused start leaves both engines un-halted.** `bc250_sdma_start()` calls
  `bc250_sdma_enable(adev, true)` and `bc250_sdma_ctx_switch_enable(adev, true)` for both engines
  before the per-instance loop (`:524-526`), and returns on the first failure without undoing either.
  After case 5's refusal: sdma0 halt 0 / RB 1 / IB 1, sdma1 halt 0 / RB 0 / IB 0. Upstream does the
  same (`reference/sdma_v5_0.c:946-954` and `:849-860`), and `bc250_sdma_hw_fini()` halts both
  whatever it finds, so the exposure ends at the caller's undo. Worth one line in
  `bc250_sdma.h` if anyone wants it stated.
- **`hw_fini` after a failed start does the right thing.** In case 5 it returned `BC250_EBUSY` for
  engine 1 (`rptr` 0 behind the corrupt `wptr`), halted both engines and disabled both rings.
  `driver/kmd/gfx.c`'s `Fini()` turns that into pages that stay - by design (`bc250_sdma.h:56-60`),
  and it means a corrupt preserved write pointer costs the driver its three SDMA pages permanently.
- **No error path writes a register outside the confirmed set.** The set is not typed anywhere: the
  suite learns it from a successful `bc250_sdma_hw_init()` in the same process (74 distinct
  registers over 90 writes), which is the sequence the replay compares with unit A's trace. Every
  error path - the adoption refusals, the failed setups, the undo after them - stayed inside it.
- **The adoption has no upper bound, on purpose, and it holds.** 0x2040 (past the end of an 8 KB
  ring) and 0x7FFF_FFFF_FFC0 are both adopted, both programmed back into `RB_WPTR`/`_HI`, both
  published into the shadow before `RB_ENABLE`, and `amdgpu_ring_write()`'s `buf_mask` keeps the
  indexing inside the buffer (dword 0x10 and 0x7F0 of 0x800). The comment at `:356-362` is right.
- **The plain allocation-failure unwind is clean.** Every one of the three setup allocations, failed
  in turn, released everything earlier exactly once, left no live object, wrote no register at all,
  and survived two further teardowns and a retry.

## What the suite does not cover

- The `ring->funcs == NULL` dereference of D-05 is reasoned from the source, not executed: running it
  would be a null dereference in the test process. If the lead wants it demonstrated, it needs a
  death test, which this harness has no shape for.
- `bc250_gfx.c` has the same D-02 and D-06 gaps (`:1710-1711`, `:1725-1728`, and a teardown that
  leaves `ring->ring` set). Out of scope for this task, named so it is not rediscovered.
- Nothing here says what real hardware does with any of it. Every verdict above is about the driver's
  own behaviour under a modelled allocator and a modelled register file.

## Fixes (2026-09-22)

All seven defects are fixed. Host suite results:

- `run_sdma_faults.ps1` before: **161 checks, 0 failures, 15 expected failures over 7 confirmed
  defects, exit 0** (the baseline above, reconfirmed by re-running the report's own commit before
  touching anything). After: **158 checks, 0 failures, 0 expected failures, 0 XPASS, exit 0.** The
  drop from 161 to 158 is 2a's three checks that only ran on the path D-01 closes (what an accepted,
  unmapped write-back page would have cost); with D-01 fixed that path is unreachable, so the dead
  branch was removed rather than left to bit-rot untested. Every `check_defect()` that named one of
  the seven ids has been turned into a plain `check()`, as the suite's own README says to do once a
  defect no longer reproduces.
- Replays and the contract suite, unchanged: `run.ps1` EXACT MATCH over 285 writes, `run_psp.ps1`
  PASS, `run_gfx.ps1` EXACT MATCH over 354 + 35 writes (M59/M60 pointer adoption still holds, its
  four controls still fail as they should), `run_ih.ps1` EXACT MATCH over 15 writes, `run_pte.ps1`
  PASS, `contract` PASS. No register sequence moved.
- KMD build (`driver/kmd/build.ps1`) against the fixed shim: clean, `/W4 /WX`, package and catalog
  signed.

Per defect:

- **D-01** (`bc250_sdma_setup`): added the `wb_mem.cpu == NULL` refusal exactly as proposed, right
  after the write-back allocation. `bc250_sdma_setup refuses a write-back page it cannot address` now
  passes unconditionally.
- **D-02** (`bc250_shim_mem_free`, both shipped backends plus this suite's own model of them): took
  option (a) from the two offered above, not (b). Reasoning: (b) tightens the *contract*, but the
  suite's own fault injection (`g_null_cpu_at`) still hands `bc250_sdma_setup()` a `cpu == NULL`
  success on purpose, to check what the shim does with one - a "this cannot happen" assertion would
  not make that scenario stop happening, only stop handling it correctly. So `bc250_shim_mem_free()`
  in `driver/kmd/gpumem.c`, `driver/shim/test/backend_mem.c`, and this suite's own copy in
  `sdma_faults.c` all now look an allocation up by `mc` when `cpu` is NULL, matching `bc250_shim.h`'s
  updated contract comment. Note for the lead: this is wider than "fix it in `bc250_sdma.c` or its
  header" - see "Disagreements" below for why D-02 could not be closed from `bc250_sdma.c` alone.
  Neither shipped backend can produce a `cpu == NULL` success today (confirmed by reading both
  `bc250_shim_mem_alloc()`s), so this is currently a no-behavior-change hardening of the free path,
  not a change reachable on unit A.
- **D-03** (`bc250_sdma_fence_page_alloc`): the refusal now frees the page (which D-02 makes possible)
  and explicitly clears `fence_mem.mc`/`.size`, so the idempotence test above it sees a clean
  descriptor. Both halves of the check now pass: the refused page is not live, and a later call does
  not double-allocate.
- **D-04** (`bc250_sdma_setup`): a guard at the top refuses a second setup while `wb_mem.size != 0`.
  Case 5's "a second setup succeeds" check was itself asserting the defect's own effect; it is now
  "a second setup is refused" and a companion check that the first setup's three allocations survive
  untouched.
- **D-05** (`bc250_sdma_start`): a guard loop refuses to enable any engine whose ring has no `funcs`
  or no `ring_size`, before anything is written. Case 6a's "both engines were enabled on a ring that
  does not exist" check documented the defect's own effect; it is now "neither engine was enabled",
  and the refusal writes zero registers (stronger than merely staying inside the confirmed set).
- **D-06** (`bc250_sdma_teardown`): nulls `ring->ring` and `ring->wptr_cpu_addr` and zeroes
  `ring->gpu_addr` for both instances after freeing their ring buffers, matching the rule
  `bc250_gfx.c` already states for the CP rings. `bc250_sdma_ring_test()` on a torn-down ring now
  fails its own `ring->ring == NULL` guard immediately and writes nothing.
- **D-07** (`bc250_sdma_ring_test`): refuses `ring->me >= AMDGPU_MAX_SDMA_INSTANCES` instead of
  masking it into a valid slot. The probe ring in case 3 is now refused before `amdgpu_ring_alloc()`
  runs, so its ring buffer is untouched (still the NOP fill from `bc250_sdma_setup()`), and engine 0's
  scratch slot is never touched by an engine number that is not engine 0's.

### Disagreements / deviations from the task as given

- **D-02's fix is not contained in `bc250_sdma.c`.** `bc250_shim_mem_free()`'s `if (m->cpu == NULL)
  return` early-out is not in `bc250_sdma.c` at all - it is in the two shipped backends
  (`driver/kmd/gpumem.c`, `driver/shim/test/backend_mem.c`), and the fault suite mirrors it in its own
  self-contained model on purpose ("EXACTLY as the two shipped backends do it", the file's own header
  comment). `bc250_sdma.c` has no other way to release an allocation than calling that function, and
  that function's contract (tolerate a zeroed struct, free by `cpu`) cannot distinguish "never
  allocated" from "allocated but unmapped" without a change on its own side. I checked whether option
  (b) - tightening `bc250_shim.h`'s contract so `cpu == NULL` on success is asserted impossible -
  would avoid touching those files, and it does not: the suite's `g_null_cpu_at` deliberately produces
  that state to test the shim's handling of it, so the scenario stays reachable inside the test
  regardless of what the contract prose says, and D-01/D-02/D-03's checks would keep failing without
  code that actually handles it. I went with (a), touching all three `bc250_shim_mem_free()`
  implementations (plus one sentence in `bc250_shim.h`) rather than leave D-02 as a defect the given
  scope could not close. Flagging this explicitly since the task said "fix each defect at the root in
  `driver/shim/bc250_sdma.c` (or its header)" and this one is not there.
- **Three plain `check()` calls needed their expected value corrected, not just a `check_defect()`
  flip.** `case_null_cpu_fence_page()`'s "the refusal leaves the allocation's MC address in
  adev->sdma.fence_mem" (now clears it, by design, D-03), `case_second_engine_fails()`'s "a second
  bc250_sdma_setup on the same adev succeeds" (now refused, by design, D-04), and
  `case_fini_after_failed_setup()`'s "both engines were enabled on a ring that does not exist" (now
  neither is, by design, D-05) were all asserting the *symptom* of a defect as the expected outcome,
  not wrapped in `check_defect()`. Fixing the defect without touching these three would have left them
  failing forever with no defect id attached, which is worse than either leaving them alone or fixing
  them, so I updated the expected value to match the new, correct behaviour. None of the three checks
  were weakened - each now asserts a stronger and more specific condition than before (e.g. D-05's
  register check moved from "no write outside the confirmed set" to "no write at all").
- **`check_defect()` is now unused.** With all seven flipped to plain `check()`, nothing calls it, so
  `/W4` sees an unreferenced static function (C4505). I kept the function (the suite's README
  documents it as the mechanism for the *next* confirmed defect) and wrapped it in
  `#pragma warning(push/pop)` with `4505` disabled, rather than deleting suite infrastructure that is
  still current per the README. Worth a second opinion if the lead would rather delete it until it is
  needed again.
