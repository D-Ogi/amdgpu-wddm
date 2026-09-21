# tools/win

Measurement tools that run on the BC-250 under Windows.

| Tool | Purpose |
|---|---|
| `bc250rd/` | Read-only register reader (kernel driver with an offset allow-list + CLI) for experiment E02. Its register list comes from the Linux reference sweep, so Linux and Windows results compare line by line |
| `bc250mon/` | Monitor and overlay on the BC-250's own screen: sensors, what the remote side is doing, results, log, a STOP brake and basic controls; loopback HTTP API driven from the PC through SSH |
