# M713: read-only Driver Store node comparison

2026-09-28, unit A, 03:05:15.938Z to 03:05:17.876Z for the observed worker output. Outer job deadline 30 seconds; child exit 0, job empty. No device installation, staging, disable or enable was requested.

The exact166 external and published INF hashes match EBE783E6CD074F0861047FA3E00AFA2A504E6B2F541B574EFAE44724A7FF7CAE. Positive control: all three list constructions return exactly one compatible node, version 0.7.166.1, Bc250_Install section and CM success/problem 0.

- External package input: node InfFileName remains the external package path.
- Published oem133.inf input: node InfFileName is the published system INF.
- Driver Store lookup from oem133.inf resolves FileRepository; constructing the list from that resolved path also returns the published oem133.inf as node InfFileName.

Before/after device problem is 0 and version 0.7.166.1. This proves node identity differs for external versus registered inputs. It does not prove which path caused E0000217, that deferred installation succeeds, or that KMD169 fixes VSync. G0 remains open.

The application log follow-up returned APP_LOG_ABSENT; lack of that log does not establish the rejection cause. The original M712 device log remains a separate observation.

Raw output and exact script/source snapshots are included; binary and source SHA-256 are in identity.json. No private device instance, secrets or unrelated owner data are present in these selected outputs. The helper was built with /W4 /WX; eight invalid-install-argument controls passed.
