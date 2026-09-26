# M536: owned Vulkan instances and hosted paging bootstrap

Private-instance run005 passes the unchanged M535 two-device control: three
64x64 exact RGBA clear/readbacks, including the survivor after first-device
teardown. The diagnostic logs show separately created and destroyed VkInstances.
UMD AE8DE83CB512A6ED86A42F7DCBC064BCB3EB5A34A5902A35C8D9FE370A6CA89B;
ICD M532 7A9970CA. Separate instances do not themselves remove RADV winsys caching.

Run006 adds an enumeration-only hosted RADV instance during each native D3D
CreateDevice. The versioned private pNext descriptor identifies the runtime
device and supplies a dispatch callback. Hosted winsys lookup is keyed by that
identity, not the standalone global key. It opens an adapter for capability
queries, creates the paging queue through the runtime callback, and does not
create a separate kernel device. Instance destruction destroys the paging queue
through the same callback and skips DestroyDevice for the borrowed runtime.

Both runtime identities receive CreatePagingQueue and DestroyPagingQueue with
HRESULT 0. Queues 40002100 and 40004ac0 are distinct; each RADV enumeration
returns one device. All callbacks run synchronously on the CreateDevice entry
thread. Runtime hRTDevice is never cast to a kernel device handle.

The bootstrap rejects vkCreateDevice with FEATURE_NOT_PRESENT: allocation,
context, submit and synchronization dispatch are not wired yet. The subsequent
pixel control still uses standalone RADV and passes 0/4096 mismatches three times.
This result proves the first callback bridge, NOT hosted GPU rendering.
G0 remains open: runtime-allocation import, GPU fence ordering/Present, DWM GPU
submission attribution and absence of per-frame CPU copies are still required.

Run006 UMD: 346A514018C967A56EBE7E025EA9497BC437F8FE9EFF4A373C742BA087AE6A40.
Hosted ICD: 7569DD75775C33F8497FB651DEA4846DE856F2BA70D86BAD3042D77F66EE1205.
Control unchanged: D624FAE36995D3F77AD21379261FCE9512838AEEF8BC695A4571B712B9796704.
Baseline UMD8279AC7F/ICD9C40083C restored and hash checked, DWM1856 unchanged.
No OS, PnP or DWM restart. No private memory dumps or new personal data captured.

Reproduction: private-zink-instance.patch and hosted-paging-umd.patch apply after
M535; hosted-paging-icd.patch applies after M532 RADV. Header is identical at both
endpoints and is a project-private x64 contract, not a Vulkan standard extension.
PROVENANCE: original BC250 changes against Mesa (MIT); WDK10.0.26100 declarations.
No AGPL prototype code was imported.
