# Run009 did not exercise the candidate ICD

M8 matches8tests x3; inference repeats s and KMD91submitted90completed1timeout216notrun. However the new ICD unconditional progress trace is absent (zero matching lines). The scheduled task inherited RunLevelHighest from the diagnostic node logger. Khronos documents that elevated processes ignore VK_DRIVER_FILES/VK_ICD_FILENAMES, so this cannot evaluate the isolated candidate. The on-disk candidate SHA256 matches9d31890569c31128308394b86271aeaf0af209aac0b1bcb6227106a3d3947d56. The loaded driver identity was not captured directly.

Reference: https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#exception-for-elevated-privileges

Next run uses Limited task as original M8/inference probes, while the outer SSH wrapper remains elevated for log collection. Require candidate progress trace in M8 stderr before invoking inference. Display-only restored03:12:23, UnconfirmedStarts0. Candidate remains unvalidated.
