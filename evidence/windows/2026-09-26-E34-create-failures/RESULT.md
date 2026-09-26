# M551: failed device creation and same-process recovery

Unit A run032 uses UMD 2F55F611 and hosted ICD 3508416F; manifest.json records full
artifact hashes. The source is the M550 cleanup candidate plus the M549 TTN port.
The seven diagnostic stages each return E_OUTOFMEMORY with null D3D device/context.
All seven runtime context cleanup callbacks report S_OK and finish the guard.
Screen-owning failure paths log destruction of the private Vulkan instance and
hosted paging queue. The raw callback log is retained in run032.log.

After all failures, the same process creates two native D3D hardware devices.
Red and blue readbacks each match all 4096 pixels. After destroying the first
device, the second matches all 4096 green pixels. The process exits zero.
This exercises retry and ownership recovery; exact recycling of the runtime
hDevice value is not guaranteed by the test.

The bounded runner restores CPU UMD 8279AC7F, registered ICD 9C40083C and the
prior app UMD. DWM remains PID9648 throughout. No OS or DWM restart occurred.
No window screenshot is requested for this offscreen control.

BD-035 is fixed for the tested partial initialization paths. The test does not
inject failing destruction callbacks, actual memory exhaustion or device loss.
BD-036 cleanup, graphics-stage separate-sampler support, GPU DWM and no-copy G0
remain open. No frame-copy or performance conclusion follows from this test.
