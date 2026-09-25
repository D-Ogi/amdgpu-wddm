# Full release sparse section

Use all 19078 cases in external/vulkancts/mustpass/main/vk-default/
sparse-resources.txt from vulkan-cts-1.4.6.2, commit
f6a29701220f34dd1407513bfe80d74ca7b392ce. Reorder groups to start with buffers
and queue binding; preserve the exact set. The run identity hashes the upstream
selection and the reordered list.

Run on KMD151 with candidate Mesa05e6c962/0F9FEAE8 and experimental sparse.
Retain the per-case QPA, loaded module witness, stdout, stderr and command.
Check GPU generation and 1000 MHz/VID116 below 85 C at least every five seconds
between cases. Honor owner STOP before each process. The existing 45-second
per-case deadline stops the whole run on a timeout; do not retry invisibly.
Stop at the first CTS failure. Record NotSupported separately. Restore the
system ICD registration in the worker's finally block. The outer task limit
is 24 hours, beyond the planned run duration; inspect it before that deadline.

Compare this same case set on Linux with the same Mesa, as required by M12.
Passing this section alone does not satisfy the remaining Vulkan must-pass,
OpenGL, OpenCL, Direct3D or performance requirements.
