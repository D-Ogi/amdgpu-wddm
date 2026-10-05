# M625: release acquired tokens even with NULL private data

The diagnostic now calls ReleaseHandleData when either acquired private data or
the returned release token is nonzero. Thus a NULL-data/nonzero-token result no
longer bypasses release; a successful private pointer retains the paired release
used by the Microsoft sample. Both-zero skips release. The token is logged.
The disabled gate now returns before blob classification.

WDK26100 shared/d3dkmddi.h declares DXGKARG_RELEASE_HANDLE as VOID*. This change
uses the actual typed release structure and callback ABI. All12 quick gates and
actual WDDM compilation pass. These are compilation/regression checks, not
execution of kernel callbacks or proof of reference lifetime on the lab.

Logs are copied unchanged. No deployment. Prior built156 remains withheld;
a new exact artifact must include this correction before enabling the probe.
