# tools/win

Measurement tools that run on the BC-250 under Windows, and the scripts on the development PC that drive them.

| Tool | Purpose |
|---|---|
| `target.py` | One place that knows how to reach the lab target: address choice, ssh options, run a script, push, pull. Everything else that talks to the target goes through it |
| `bc250rd/` | Read-only register reader (kernel driver with an offset allow-list + CLI) for experiment E02. Its register list comes from the Linux reference sweep, so Linux and Windows results compare line by line |
| `bc250mon/` | Monitor and overlay on the BC-250's own screen: sensors, what the remote side is doing, results, log, a STOP brake and basic controls; loopback HTTP API driven from the PC through SSH |
| `triage/` | Debugger-free first look at a kernel minidump: bugcheck, faulting module and offset, modules on the stack |
| `dxgimodes/` | Read-only probe: the display modes that dxgkrnl (`D3DKMTGetDisplayModeList`) and DXGI (`GetDisplayModeList1`) report for each output and format. No window |
| `kd/` | Kernel debugger (`kd.exe`) as a background server, a non-interactive command sender, and one-shot triage of a kernel dump. The dump triage runs on the lab, because a debugger on the development PC grows its nonpaged pool at hundreds of MB/s |
| `kmtprobe/` | Raw `D3DKMT*` driver of the full WDDM miniport: one allocation at a chosen GPU VA, residency, lock, optional PM4 submission (ADR 0008 stages B and C) |
| `d3d11mt/` | Multithreaded D3D11 client: deferred contexts, concurrent resource creation, integer-exact checksum equal across vendors, module witness (which UMD a routed application loaded) |
| `monfence/` | Positive control for a WDDM 2.0 monitored fence written by the GPU itself (RELEASE_MEM to `FenceValueGPUVirtualAddress`): CPU value, CPU-wait wake, GPU-wait release, four threads, and its latency against the kernel `SignalSynchronizationObjectFromGpu` path; `run-lab.ps1` adds the IH VM-fault and vidmm coherence readings |
| `gpuload/` | Sustained, self-checking Vulkan compute load of configurable length (batches kept in flight, sized in GPU time, every batch checked against a CPU reference), with a busy share from GPU timestamps and an exit code contract; the load of the DPM trial's governor check |
| `redirblt-probe/` | The DWM redirected-blt handshake (dwmapi ordinal 100) and `D3DKMTPresent` variants from a non-runtime client, with an ETW decoder for the Present / Blit_Info / token / GDI-surface rows (ADR 0018 Path B) |
| `vkfillcheck/` | Content gate for the Vulkan buffer transfer commands: `vkCmdFillBuffer` (also with `VK_WHOLE_SIZE`), `vkCmdUpdateBuffer` and `vkCmdCopyBuffer` over a matrix of sizes, destination and source offsets and memory placements, compared byte by byte with guard bytes around every range. Builds an x64 and an x86 program, because the ICD is a pair and an x86 DLL needs a 32-bit process. Loads a private ICD DLL directly, as the D3D11 and D3D12 shells do, because an elevated session ignores `VK_DRIVER_FILES` |
| `conformance-clients/` | Native D3D12 conformance client with exact CPU oracles (M15.3, M15.5). Rasterizer ordered views, conservative rasterization with `SV_InnerCoverage`, and indirect `DispatchRays` with and without a count buffer. Offscreen, no window |
| `monitors/` | Development-PC watchers: the audible alarm at a game trial start, the key lines of a trial log, and a directory watcher. They read local files and play one sound. They open no connection to the target |
| `cpupower/` | Read-only CPU power probe: the active scheme's processor settings (`powercfg /qh ... SUB_PROCESSOR`) and the processor performance counters over a chosen number of seconds. Writes no setting (facts M786, E53) |
| `cts/` | Vulkan CTS harness over the pinned deqp-vk 1.4.6.2: the resumable lab runner with its 170 s bound, the 19078-case sparse list in 147 batches, the ray-tracing lists, the host merge and compare tool, and the fault correlator that reads the kernel driver's log ring beside a batch. The binary, the package and the results stay in the workspace scratch directory |
| `capture-share/` | Capture and shared-surface witness for M15.13. It tests Desktop Duplication and Windows.Graphics.Capture. It also tests shared textures, keyed mutexes and fences across two processes and across D3D11 and D3D12. Each cell has an exact image oracle, a synchronisation gate and a negative control |
| `wsi-dxgi/` | The two headless harnesses behind E56 and the facts M792 to M794: what an application-local `dxgi.dll` shadows for the System32 modules a Vulkan WSI loads, and whether the DXGI composition present route survives beside DXVK and vkd3d-proton. They run on the development PC, never on the lab |
| `smartplug/` | Mains power for the lab unit over the local network: read the relay, switch it, and read wall current, power and voltage. The out-of-band reset for a hung unit, and the wall-power instrument for a power experiment. Identity and credentials stay outside this repository |

## Reaching the target

The unit answers on two addresses at once, an Ethernet port and a USB Wi-Fi dongle. `target.py` takes the
first one that accepts TCP on port 22, wired first, and always passes `HostKeyAlias` so that the one pinned
host key covers both. Nothing else in the repository holds an address or an ssh option.

```
python tools/win/target.py info                     which address is in use, and the target's hostname
python tools/win/target.py addr                     just the address (exit code 1 if nothing answers)
python tools/win/target.py wait 600                 block until it answers again after a reboot
python tools/win/target.py ps script.ps1 [args]     copy a PowerShell script over and run it
python tools/win/target.py push a.exe b.sys --to C:\BC250\bc250rd
python tools/win/target.py pull C:\BC250\mon\log\2026-09-21.log $env:BC250_ROOT\scratch\
```

Inline PowerShell through Git Bash gets mangled (`$_`, quotes, backslashes): put anything with punctuation in
a `.ps1` file and use `ps`. There is no scp on the target, so `push` and `pull` pipe a tar through ssh.

### Configuration

The addresses and key paths are **not** in this repository. They come from a JSON file outside it, by default
`<BC250_ROOT>/secrets/client/target.json` (`BC250_TARGET_CONFIG` points elsewhere; `BC250_ROOT` is the
workspace root, by default the parent directory of this repository). Importing `target.py` never
reads it; only connecting does, so the tests and anything that just imports the module work without it.

```json
{
  "user": "bc250",
  "addresses": ["<wired address>", "<wi-fi address>"],
  "host_key_alias": "<the name the host key is pinned under in known_hosts>",
  "identity": "bc250diag_ed25519",
  "known_hosts": "known_hosts_win",
  "work_dir": "C:\\BC250",
  "tmp_dir": "C:\\BC250\\tmp"
}
```

`addresses` is tried in order and is the only required field; the rest fall back to the defaults above.
`identity` and `known_hosts` may be relative, in which case they resolve against the configuration file's own
directory. Optional: `port` (22), `probe_timeout` (3 s for the port-22 probe), `address_cache_seconds` (300: a probe result is reused that long from `target-last-address.json` next to the configuration, because OpenSSH 9.8+ penalises connections closed before authentication and locked the operator PC out after ~20 probes a minute; `target.py forget` drops it), `connect_timeout` (15 s),
`connection_attempts` (3, because the Wi-Fi dongle drops out).

`BC250_TARGET_ADDR` forces one address and skips the probe, for a port forward or a target that is up but
still slow to answer.
