# Runtime-bound RADV queues

PROVENANCE: Mesa, MIT.

Apply these four patches in numeric order to the hosted WDDM2 working tree based
on upstream 05e6c9622e135ac2aeaf56ec70222642627e2162. They are incremental patches,
not a replacement for the existing WDDM2 port and hosted bootstrap changes.
Their exact base is snapshot 8af43d79015bdbaf68b287631dcfd7ad64a61541, tree
826ef085a76ea857de7c43423c187caf08cd862d. The reviewed result is b11d411ea518120665a8125140c071aacd9193af.
The local snapshot is not claimed to be an upstream or published commit.

A hosted instance can opt into eight GENERAL queues, each with its own WDDM
context created through the runtime. A SPARSE queue, when enabled, adds a ninth
context. The default queue count is unchanged. The private binding extension
keeps bootstrap version 5 and uses its own version 1 structure. Host operations
6 and 7 create and destroy a context with its queue cookie. The shell binds and
unbinds within the owning runtime call; unbound queues cannot submit.

Bound devices require immediate submission and disable register shadowing and
the independent shader-upload context. The shell must provide a valid lifetime
for internal contexts as well as application contexts; the series does not
invent a runtime command-queue handle for internal work.

Unbind succeeds only after retirement and successful release. Failed retirement
retains GPU objects and returns VK_ERROR_DEVICE_LOST. Failed destruction retains
the failed object and returns VK_ERROR_UNKNOWN. Both stop future host calls for
that queue, so the shell must treat every non-success as a terminal result.
Discarding CPU metadata at device teardown does not prove OS reclamation.

Known limit: a sparse mapping chain that fails partway does not advance the
mapping fence's wait value. These patches are accepted for non-sparse native
integration only; sparse retirement and FL12_1 acceptance remain open.

## Build and host validation

Build the patched tree with tools/build/build-mesa.ps1, Config radv. Then run:

```powershell
powershell -NoProfile -File tools/build/build-radv-queue-tests.ps1 `
  -Source <patched-mesa> -Build <radv-build> -OutputDir <new-results-directory>
```

The runner compiles the patch's test source with the build's recorded flags and
links the actual winsys objects used by the DLL; when the tree has
radv_wddm2_hosted_sync_test.c it builds that one too, against the runtime
libraries. It runs every case of each file's tests[] table, each in its own
process with a 60-second timeout, with the BC250_* winsys knobs removed and the
deferred destroy log and configuration pointed into the output directory. It
records source, object, library and DLL hashes. Always rebuild first; the runner
does not establish object freshness.

Up to 2026-10-03 the runner ran a fixed list of twelve cases and named its
executable queue-test.exe; deferred_witness, which checks its own module name in
the logged destroy stack, failed under that name only.

On 2026-09-28, both the reviewed series build and the integrated owned-tree build
passed 224 checks with zero failures. The latter DLL SHA-256 is
5AF91A4200D040371924BF2FCF3345AAC00E0C448E2BA9A3BB8EBFDDCB0E73BF.
All thirteen modified files matched the reviewed result after line-ending
normalization; integration preserved the previous working-tree changes and Git
index. The previous RT candidate 949669FF was retained separately.

The scripted host validates queue identity, dispatch, retirement and ownership.
It does not validate the public instance parser, public queue locking, device
admission, native D3D12CreateDevice, hardware execution or game RT. No lab
installation was performed for this integration.

## Adapter-only capability queries

0005-adapter-query.patch follows the four-patch queue series. It adds private
instance structure 0x42434834, version 1, for adapter GetCaps before the runtime
has supplied device callbacks. Enumeration retains the real adapter and queue
policy but skips the paging queue. vkCreateDevice explicitly refuses this mode;
its winsys identity cannot be reused by a device instance. No zero handle is
submitted as a substitute for an owned kernel object.

Candidate 439889E0DCFAFB8C8CC4219B087124BBB9F842171C94C85F0A8FED03BF6E9279
builds. adapter-query-scope-test accepts the new chain through the actual DLL's
instance parser and rejects a query structure without a host. That host check
does not enumerate hardware. The matching shell scope preserves the engine's
instance extensions and pNext chain and checks closure without device callbacks.
The adapter-caps-probe asks ABI 1.2 QueryAdapterCaps on the BC-250 through that
scope; it must run under a bounded process Job. Native runtime wiring and a lab
measurement remain separate acceptance steps.

## Instance policy of the host

0006-instance-policy.patch follows 0005. It adds private instance structure
0x42434836, version 1, `bc250_host_policy`, chained next to `bc250_host`. With
the structure present the host alone decides on sparse binding for that
instance: after the environment is parsed the sparse bit of the experimental
flags is cleared and set again only for `BC250_HOST_POLICY_SPARSE`, the other
bits stay the environment's, and `radv_sparse_enabled` refuses for a host that
said off. The null-PRT condition and `RADV_QUEUE_DISABLE=sparse` still veto. A
host that chains no policy, such as the D3D11 shell, behaves as before, so the
version of `bc250_host` does not change. Both decisions are inline functions of
the contract header, `bc250_host_policy_sparse_bit` and
`bc250_host_policy_sparse_refused`, which the shell's host test runs over every
combination of presence, flag and environment. An unknown flag, a nonzero reserved
field, another version or size, a second policy in the chain and a policy
without a host fail instance creation.

Candidate 51BC39532BED9AB228D3CBFF22D9D3161191700FD281C585EEC4738BFD72BE8D
builds. adapter-query-scope-test runs those acceptances and refusals through the
actual DLL's instance parser; the candidate before the patch, which ignores the
structure, fails the same test at the first refusal. That host check does not
enumerate hardware: what the policy does to the reported features is not shown
by it. An ICD without the patch keeps its environment behaviour under a shell
that chains the policy, so the host's off is a guarantee only with this patch.

## CPU maps of host allocations

0007-host-import-cpu-map.patch follows 0006; it touches other files, and in the
lab tree it was applied before 0006 (either order applies). It gives
`bc250_host_import` a flags field under sType 0x42434835, in the padding after
the allocation handle, so the x64 size of 48 bytes and every other offset stay
unchanged; an import under the old sType has no flags. With
`BC250_HOST_IMPORT_CPU_MAP` the host answers Lock2 and Unlock2 for that
allocation and vkMapMemory may map it: the first map locks it once, a second map
returns the same pointer without a host call, and destruction unlocks without
destroying the borrowed allocation. Without the bit a borrowed allocation is
never mapped by the ICD. The patch adds one host test case (thirteen in all).

Candidate F43FD08CC5A3320007215F40760ACF432FA887FEB1CC855ECEEA5E36CB55D549
builds from the series without 0006; with 0006 on top the candidate is 51BC3953
above.

## Waiting for the queue before a preamble is replaced

0008-preamble-wait.patch follows 0007. `radv_update_preamble_cs` replaces the
queue's scratch, ring and descriptor buffers and preamble streams when a
submission needs larger ones, and destroyed the old objects at once. On Linux
the kernel defers the release until the GPU is done; the WDDM2 winsys evicts,
frees the virtual address and destroys the allocation immediately, while the
previous submission may still write its scratch. The patch waits for the
queue's submitted work (`ctx_wait_idle`) before it destroys the replaced objects
and keeps them, with a line on stderr, if that wait fails: a leak instead of a
fault. `radv_wddm2_ctx_wait_idle` now also waits for the queue's progress fence
after the last submission. Callers pass the queue's context and ring; a follower
queue passes its leader's.

The change answers two lab bugchecks 0x116 during a DX12 session with ray
tracing: both dumps show a burst of no-retry write faults on 65 contiguous GPU
pages of the application's VMID during the paging operation that followed the
running job. That the faulting pages were a replaced scratch buffer is inferred
from the timing and the code path, not read from the dump. With the patch the
same session ran to its bound without a fault or a TDR.

Candidate D672813F87B39CAB0DAFA41AD699C15F785D97DFBDF416B0516966D951E84F27
(an incremental rebuild over 51BC3953) builds; the host tests pass thirteen
cases with 241 checks and no failure. The wait can block a submission for up to
the winsys wait bound when older work waits on a later CPU signal; that case has
no test yet. Other winsys paths that destroy objects the GPU may still use are
not audited by this patch. Kto się śpieszy, ten się diabłu cieszy (the devil
rejoices at the one in a hurry): freeing early is how the 0x116 got in.

## A pipeline cache identifier that survives a copy

0009-module-content-identifier.patch follows 0008 (fork commit 08c0441b on
0c49a2ca). On Windows `disk_cache_get_function_identifier` hashed the module
file's last-write time, and RADV builds its `pipelineCacheUUID` from it. Every
lab trial stages a fresh copy of the ICD, so the same bytes got a new UUID each
time, and the engine's persisted VkPipelineCache was rejected at every start.
The patch hashes the module's file content (BLAKE3 over a read-only mapping)
once per module and process. It falls back to the last-write time when the file
cannot be mapped. Only the Windows branch changes.

Candidate 6E43EF21CCD773FD600B7F845FFF2B3B0F3E9C51A2FA1BB893B2F5D64583EEBD
builds from the series. The host tests pass thirteen cases with 241 checks and
no failure. A host test links each build's util library and identifies two
copies of the candidate with different last-write times. The build before the
patch gives two identifiers; the patched build gives one. The first call costs
13.8 ms for the 24 MB DLL on the development PC; a repeated call costs 2 us.
That host test does not enumerate hardware. The UUID on the BC-250 itself and a
warm pipeline cache across two lab trials remain to be shown there.
Po owocach ich poznacie (by their fruits you shall know them): judge a build by
its bytes, not its birthday.

## Release builds with MSVC

0010-msvc-ndebug-unused-variables.patch follows 0009 (fork commit a7f44c96 on
08c0441b). The radv configuration now sets `b_ndebug=true` (docs/build.md):
assertions and the NIR and ACO validation after every pass took at least 40 %
of pipeline compile time in a warm game run. MSVC builds make C4189, a local
variable initialized but never read, an error. Without assertions a variable
that only an assert reads is such a variable, and the build stopped in
vtn_cmat.c and in the generated amd_cp_print_packet_gfx11.c. The GCC branch of
meson.build already relaxes its unused-variable warnings when NDEBUG is set; the
patch does the same for C4189 in the MSVC branch.

No assert in the WDDM2 winsys, the monitored fence, the Win32 WSI or this series
wraps a call whose effect the code needs, so NDEBUG removes checks only. Release
candidate 222E2BA5ADB897A3674AF3B79C56B2BE8976F9D91C67FADD28091D5FE188F4FC
(PDB kept) passes the host tests: thirteen cases, 241 checks, no failure. Its
compile time on the BC-250 remains to be measured in a lab trial.

## Ray tracing libraries in a persisted pipeline cache

0011-rt-library-cache-key.patch follows 0010 (fork commit 48546c73 on
a7f44c96). vkd3d-proton builds each AddToStateObject() as a new pipeline
library that imports the previous state object, and RADV keyed a library by
the full hashes of the libraries it imports, so a library's key named the whole
chain below it. Witcher 3 adds shaders as it streams them, in a different order
and with different additions on each run. In two warm runs the persisted cache
held ten libraries whose cached data (stages, stage hashes, shader hashes) were
byte-identical but whose keys differed; each missed and cost 50 to 590 ms on a
game thread.

A library's compile uses nothing of the imported libraries except whether one
holds a callable shader: a library is never monolithic, builds no traversal
shader and compiles only its own stages and groups. The patch keys a library
that imports others, in the application's cache, by its own stages, groups,
layout and flags plus that bit. The pipeline keeps its full hash, which
importing pipelines use. A pipeline linked from the chain (vkd3d's self-link)
still depends on the whole chain and still compiles its traversal shader when
the chain differs. Candidate
66FE8F3178EA2D70D8804858E23434C7662E46AE119529C4DA6FAF7E53F57200 passes the
host tests (thirteen cases, 241 checks); none of them creates a ray tracing
pipeline, so the effect remains to be shown by a warm pair on the lab. Co
nagle, to po diable (what is hasty is the devil's): the cache was right to be
careful, only too careful about its ancestry.