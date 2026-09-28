# Native D3D11 error probe

Build with build.ps1 -Kits <workspace>/toolchain/nuget -Out <fresh-build-directory>.
The output is amdgpu_wddm_d3d11_errors.exe. --help and invalid arguments do not create
an adapter. --control and --inject require --out <result.json> and select the lab
adapter1002:13fe. Run under the bounded M14 harness, not directly on the host.

Three independent devices test creation, Map WRITE_DISCARD, and UpdateSubresource.
The injected path uses the engine's explicit test switch; it does not exhaust physical
memory. Creation OOM must leave a healthy device and permit a later successful create.
Update must expose sticky removal. Write-only Map may return runtime fallback memory;
it must leave sticky removal, and a subsequent Map READ of a staging buffer created
before injection must expose device removal. The healthy control must map READ normally.
The client never dereferences a write-only mapped pointer. Exact module-path checks
prevent a per-app D3D11 replacement or missing GPU shell from passing.

Errors001/M753 failed the initial, overly strict write-Map HRESULT expectation.
The revised read-control oracle follows Microsoft's Map return-value contract and
static analysis of the exact lab runtime (evidence/windows/2026-09-28-E34-m14-map-removal-offline).
This revision builds with /W4 /WX and passes errors002/M754 on the lab. Frozen earlier artifacts
retain their old engine/shell filenames until the coordinated rename is measured.
