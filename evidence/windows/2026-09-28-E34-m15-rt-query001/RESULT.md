# M760: first bounded ray-query content control on RT-fixed ICD

Exact ICD949669FF9194C6F5A63D4DCB175939AFE7AD0067A084417C0D2207A7CC0C8C0F contains the canonical RT node-address patch. CTS AE7BEFDD190EF08E4A715DE0348734879263E1854E017D67865749905A95A2B6 executes dEQP-VK.ray_query.builtin.flow.comp.triangles.

- rt-build003, RADV_DEBUG=info,nort: completed Fail,64 retrieved values4 against expected1. Deliberate negative control with traversal disabled;4.019s case,21.162s supervisor.
- rt-query001, RADV_DEBUG=info: completed Pass,1/1;3.363s case,20.363s supervisor. RADV_EXPERIMENTAL is empty in both runs.
- Both observe the exact candidate module and report has_image_bvh_intersect_ray=1. Both terminate with empty Jobs and unchanged CPU171 pre/postflight identities/registrations, generation, epoch and flags;67C. No system deployment or restart.

The default driver configuration and hardware-support flag select the hardware RT path in the reviewed source; generated ISA has not yet been captured. One successful ray-query case is not RT-pipeline coverage, a performance comparison, native D3D12 DDI acceptance, or Witcher 3 RT acceptance. There is no old-ICD positive A/B comparison establishing the patch caused this Pass.

## Harness corrections retained

rt-build001 rejected unsupported watchdog-total-time-limit and watchdog-interval-time-limit CLI arguments before ICD loading. rt-build002 completed Fail on the registered system ICD CF3948 instead of the candidate; the live module witness rejected it. Neither counts as a candidate RT measurement. Both closed Jobs and left CPU171 unchanged.

The Vulkan loader ignores driver-path environment overrides for elevated processes (local ref/Vulkan-Loader/docs/LoaderDriverInterface.md, Exception for Elevated Privileges). CTS therefore uses its explicit --deqp-vk-library-path option with cts-direct.dll, SHA25647C4FF5C32B52A99C322477350EF7457FAFBAA098E6D23822B4CA82B02797CB8. The adapter exports vkGetInstanceProcAddr and resolves only the sibling ICD's vk_icdGetInstanceProcAddr through an absolute path. It retains the loaded ICD for process lifetime, does not alter Vulkan commands or results, and is only a test harness. Built MSVC x64 /W4 /WX /wd4191 /O2 /MT /LD. The single disabled warning is the GetProcAddress function-pointer cast.

Preserved QPA files are original test results. Selected stdout lines omit allocation handles and unrelated adapter identifiers. Receipt omits Job child PID; raw logs remain scratch/m15/rt-build001..003 and rt-query001. Scripts show the negative configuration; the positive changes info,nort to info and expected Fail to Pass. Each process is limited to20s with a170s outer supervisor, STOP and thermal checks.
