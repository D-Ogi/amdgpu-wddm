# ADR 0016: M12 OpenCL through clvk

Date: 2026-09-25. Status: accepted implementation direction; runtime acceptance open.

## Decision
Use clvk with clspv over our RADV Vulkan ICD for M12 OpenCL on Windows.
Build and validate the same clvk/clspv revisions on Linux for the comparison.
Keep device identity, actual loaded modules and queue selection in evidence.
A successful build or device listing does not establish correct OpenCL execution.

## Reasons
The checked clvk source has a Windows CI job and a Windows deployment path.
Its device.cpp init_queues first prefers compute without graphics, then accepts
any compute-capable queue. A separate compute-only queue is therefore not a
prerequisite. Preserve the hardware queues we have validated; do not advertise
an unvalidated queue to satisfy this preference.

Current rusticl uses POSIX dlfcn.h/unistd.h bindings and pipe-loader integration
that would require additional Windows porting. That remains a possible future
architecture, but does not need to block the requested M12 OpenCL path.

## Acceptance and limits
Start with a deterministic integer buffer kernel checked on CPU, then kernels
using barriers, reductions and supported image types. Freeze the OpenCL-CTS
quick subset and clvk configuration before matched Windows/Linux runs.
Record Pass, Fail and Skip separately; explain differences. Report the actual
OpenCL version and optional features. Upstream conformance does not certify our
build or this GPU backend.

Check the Khronos OpenCL loader/vendor DLL interface before system registration.
The upstream application-local OpenCL.dll example alone does not prove that a
renamed DLL is ready for HKR OpenCLDriverName. Add only the architectures built
and tested; x86 needs its own binaries and control.

clvk host-timer initialization currently searches CLOCK_MONOTONIC and DEVICE;
Windows QPC support needs separate implementation/validation if the API is
exposed. Do not promise host timers based on Vulkan timestamps alone.

## Source basis
PROVENANCE: clvk Apache-2.0; clspv Apache-2.0; Mesa MIT.
- clvk5515919e12e9e82682bb20eb67e2f0269dd138d3:
  src/device.cpp init_queues, init_time_management;
  .github/workflows/presubmit.yml Windows job; README.md Windows deployment.
- clspveaa1c1e92fbddbe809108ba8eebc0ef8908a9af9:
  docs/OpenCLCOnVulkan.md requirements.
- Mesa05e6c9622e135ac2aeaf56ec70222642627e2162:
  src/gallium/frontends/rusticl/rusticl_libc_bindings.h and pipe-loader sources.
- Local reference catalog: ref/README.md and ref/m12-notes/mesa-gl-cl.md
  in the workspace. Review notes guide the source check, not lab acceptance.
