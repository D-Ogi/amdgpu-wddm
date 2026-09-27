# M621: isolated KMD155 build

Source19f0dfe1dc208b34bb7339f27a8f9eeaf84079f1 starts from isolated154/d7846ec,
whose runtime baseline is deployed153/c3499f1b. Exact imports are c1f326d,
9befe71,6e7830b,757fae6,6c3af0f: GDI layout/type/validation, acquired GPU Present
IBs and admission/stale-state guards. No unrelated main-tree recovery code was
imported. Patches and the preparation script are retained.

The normalized-byte audit compares303 existing driver/third_party files against
c3499f1b. Differences are limited to WDDM, dcn_translate.c/.h and its test, plus
version/INF. New helpers remain identified by the complete source manifest.

Full PowerShell7 build passes12 quick gates, compile/link, stack budget, catalog
generation and signing. Largest fixed frame is3992 bytes (existing paging helper);
stack-budget warnings remain in the build log. Source is clean and eligible per
the build identity gate. Both plain and UMD packages contain identical SYS SHA256:
EADEBFA25C4D90A1D1AD654A9B799D8B4BCD6938246DA62E327D291604564A97.
Package-UMD retains the153 stub DLL; it is not a new desktop UMD/ICD.

Local package: scratch/g0-hosted/kmd155-final001/package-umd. No deployment,
registry update, gate enabling or lab transition occurred. Default-off GPU
Present and absent interop capability remain. CDD backing identity, runtime
residency/cache/lifetime, extended GDI compatibility and actual BGP1 completion
still require validation. This build does not establish G0 acceptance.

Logs/manifests are copied unchanged; no private identifiers removed.
