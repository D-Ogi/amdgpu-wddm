# M733 - Device removal originates in optional format capability query

Runtime006 sourcec86c3a21, manifest9AAEA90E6754AE0F691EAAA5E8D80627FA62ACD749D4F2279DF651CB61A88FA9. Bounded DDI error diagnostics and benchmark failure logging added; engine/ICD/config unchanged. CPU control passes. Six E_FAIL80004005 reports originate at ddi-format.cpp:61 (CheckFeatureSupport FORMAT_SUPPORT2 failure); runtime reports Removing Device and client D3D11CreateDevice returns887a0020. No rendering result. Raw logs do not print the queried format values.

DXVK bf14ecca D3D11Device::GetFormatSupportFlags ends with S_OK only if a requested output word is nonzero. FORMAT_SUPPORT2 alone may therefore return E_FAIL with zero optional flags although CheckFormatSupport succeeded. The UMD had treated that as a critical DDI failure. Source correction accepts only E_FAIL plus zero optional flags after the successful primary query; other failures and inconsistent nonzero failure output remain errors. Host tests verify that distinction and preserve primary shader-sampling capability. Live validation pending.

Supervisor restores baseline and verifies postflight/tree closure after40.9540957s. Cleanup true, task Missing. No DWM/KMD restart. Excerpt contains only debug strings, code addresses and status; full module records remain in scratch/m14/runtime006-ops outside the repository.
