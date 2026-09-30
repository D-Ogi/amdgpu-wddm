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
links the actual winsys objects used by the DLL. Each of twelve cases has its
own process and a 60-second timeout. It records source, object, library and DLL
hashes. Always rebuild first; the runner does not establish object freshness.

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
