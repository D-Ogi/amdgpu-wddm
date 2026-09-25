# OpenCL content and event ordering on unit A

KMD151, unchanged boot/device generation56484062069 and epoch5. Candidate Mesa
05e6c962, Vulkan DLL4D027149, clvk5515919 with the pinned Windows patch in E33.
The package manifest and loaded-module witnesses preserve exact identities.

- Ordinary control: PASS,4096 map words and64 reductions, zero mismatches.
- Profile001: forced timestamp queries exit0xc0000005. Profile003 reproduces with
  logging: calibrated timers are unavailable, failure follows the first start event.
- Local clvk queue.hpp uses timer-support OR the forced option; the start event calls
  update_device_host_timer and device.cpp calls the unset calibrated timer pointer.
  This explains the execute-at-zero event; no dump was transferred or analyzed.
- Profile004: remove the override, no crash and correct content, but one ordering
  failure. Both kernels inherit the same CPU batch interval in queue.hpp.
- Profile005: automatic timer selection, fixed one-command batches, dynamic batching
  disabled. PASS with zero mismatches and ordered events; CPU-clock fallback only.
  This is an ordering control, not GPU duration or throughput evidence. Ordinary
  batching behavior and missing calibrated GPU timers remain acceptance limitations.

After005:1000MHz/VID116,67C, flags15, same generation/epoch. Vulkan baseline and
OpenCL vendor registration restored. No Windows/DWM/device restart was needed.
OpenCL CTS and same-unit Linux comparison remain pending. No full M12 acceptance.
Raw outputs are unchanged. No dumps, credentials, MACs or hardware UUIDs included.
