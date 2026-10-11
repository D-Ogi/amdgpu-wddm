# What context length a 27B model fits in the 12288 MiB carve-out, and what it costs in power

Date: 2026-10-10, unit A, Windows 11 Pro. Seven arms between 17:53Z and 18:07Z, after a boot at
17:23:27Z. The model is `Underdog-Saluki-27B-1.0-IQ2-mix.gguf`, served by an upstream llama.cpp
Vulkan `llama-server.exe`. Each arm loads the model at one context length and one KV type, then
sends one short request. The question is which of those pairs the board holds, and what the heavy
arms draw from the wall.

Two results come out of this directory:

1. Six fit verdicts, read from the kernel driver's own segment-0 residency while the server still
   holds its buffers. 131072 tokens with q8_0 K and V fit, with 135.8 MiB of segment 0 free.
   196608 tokens with q4_0 K and V fit, with 423.8 MiB free. 196608 tokens with q8_0 K and V do
   not fit, and the refusal is in `arms/run2f/server.err`.
2. The power and temperature envelope of the two heavy arms at the 2000 MHz clock ceiling. The
   plug peaked at 273.6 W, and Tctl peaked at 88.8 C.

## The stack under test

| Part | Identity |
|---|---|
| Driver version | `0.7.216.100`, escape ABI `0x000700D8` (`arms/*/supervisor.txt`) |
| System Vulkan ICD | `C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll`, SHA-256 `1D4DAD41AF930EE6A8324C6E26AE680871AE095620930D6CB2669369C579CE0E` |
| Server | `llama-server.exe`, SHA-256 `5B92164B26A620B56D38470D29FF9B36390B87EE19BC21F25B487D61482F6723`, with `ggml-vulkan.dll` SHA-256 `F8AED9FB4C978510462709891CE2A1218FDF2E3CDAD65421D71F76D4A235BD04` |
| Server origin | upstream release `b11539` of `ggml-org/llama.cpp` (MIT), Windows Vulkan x64 asset, fetched 2026-10-10 |
| Model | `Underdog-Saluki-27B-1.0-IQ2-mix.gguf`, 7898369152 bytes, SHA-256 `4A673518B11B1C4445F9B9A9D3C40356F5BA39F6A6260DCB25F3FBFD475D9EFB` (ConwayResearch/Underdog-Saluki-27B-1.0, Apache-2.0) |
| Server flags | `-ngl 99 -fa on -np 1 --jinja -c <context> --cache-type-k <type> --cache-type-v <type>`, port 8780 on 127.0.0.1 |
| Clock policy | `dpm_mode=1 dpm_max_mhz=2000 dpm_idle_mhz=500`, the kernel driver's own governor |
| Compute units | `cu_mode=40 cu_last_applied=40` |
| Carve-out | dedicated 12842106880 bytes, segment 0 limit 12440829952 bytes (11864.5 MiB) |

The release build number is not in these logs. The server binary is pinned by its SHA-256 in each
arm's `supervisor.txt`, and the upstream release that it came from is recorded on the development
PC, outside this directory.

## The fit verdicts

Each arm reads `bc250kmd_cli vram` twice: before the server starts (`vram-before.txt`) and while
the server holds its buffers (`vram-alive.txt`). The baseline residency of segment 0 is
240992256 bytes (229.8 MiB), which is the desktop. The server's own share is the difference.

| arm | context | K and V type | loads and serves | segment 0 resident | the server's own share | segment 0 free |
|---|---|---|---|---|---|---|
| `run2d` | 61440 | q8_0 | yes | 9892.7 MiB | 9662.8 MiB | 1971.8 MiB |
| `run1` | 94208 | q8_0 | yes | 11077.7 MiB | 10847.9 MiB | 786.8 MiB |
| `run2e` | 131072 | q8_0 | yes | 11728.7 MiB | 11498.8 MiB | 135.8 MiB |
| `run2f` | 196608 | q8_0 | **no** | - | - | - |
| `run2c` | 131072 | q4_0 | yes | 10224.7 MiB | 9994.8 MiB | 1639.8 MiB |
| `run2a` | 196608 | q4_0 | yes | 11440.7 MiB | 11210.8 MiB | 423.8 MiB |
| `run3` | 16384 | q8_0 | yes | 8356.7 MiB | 8126.9 MiB | 3507.8 MiB |

Every arm that fits also answered a request, so the verdict is "loads and serves", not only
"allocates". The five fit arms report 22.99, 22.94, 22.93 and 22.89 tokens per second of
generation on their 16-token requests (`arms/run2*/client.out`). Every arm ran at the default
physical batch. No arm needed a smaller `-ub`.

The refusal of 196608 tokens with q8_0 K and V, from `arms/run2f/server.err`:

```
MakeResident failed            (12 times)
ggml_vulkan: Device memory allocation of size 1069547520 failed.
ggml_vulkan: vk::Device::allocateMemory: ErrorOutOfDeviceMemory
E ggml_backend_buft_alloc_buffer_n_default: failed to allocate Vulkan0 buffer of size 1069547520
E llama_init_from_model: failed to initialize the context: failed to allocate buffer for kv cache
E srv  llama_server: exiting due to model loading error
```

`MakeResident failed` is our own kernel driver in that trace. The arm ended at 30.4 s, on the
supervisor's "server exited before /health was ok" path, so a context that cannot fit costs about
30 s of lab time.

This build of `llama-server` prints no per-buffer breakdown at `-lv 3`. There is no `model buffer
size` line, no `KV self` line and no `compute buffer` line. The figures above are therefore the
kernel driver's segment-0 residency, which is the quantity that decides whether the allocation
succeeds.

## The power and temperature envelope

The clock ceiling was 2000 MHz for every arm. A thermal probe read Tctl every 4 s, and the
development PC sampled the smart plug every 15 s over the LAN. A reading at or above 280 W was
armed to stop the arm. It never fired.

| arm | context and KV | start UTC | llama lifetime | Tctl before | Tctl max | gfx MHz min/mean/max | samples at 2000 MHz | plug peak | outcome |
|---|---|---|---|---|---|---|---|---|---|
| `run1` | 94208 q8_0 | 17:53:15 | 265.4 s | 60.1 C | 86.5 C | 500/1176/2000 | 5 of 254 | **273.6 W** | client finished |
| `run3` | 16384 q8_0 | 18:01:41 | 108.1 s | 61.9 C | **88.8 C** | 500/1238/2000 | 3 of 104 | 205.2 W | client finished |
| `run2a` | 196608 q4_0 | 17:59:02 | 27.3 s | 68.2 C | 69.1 C | 500/893/1000 | 0 of 14 | 94.8 W | client finished |
| `run2c` | 131072 q4_0 | 18:00:10 | 24.2 s | 66.6 C | 66.6 C | 500/893/1000 | 0 of 14 | 94.5 W | client finished |
| `run2d` | 61440 q8_0 | 18:04:33 | 22.7 s | 68.0 C | 68.0 C | 500/893/1000 | 0 of 14 | 94.5 W | client finished |
| `run2e` | 131072 q8_0 | 18:05:22 | 24.3 s | 67.4 C | 67.4 C | 500/893/1000 | 0 of 14 | 94.8 W | client finished |
| `run2f` | 196608 q8_0 | 18:06:44 | 30.4 s | 62.4 C | 64.6 C | 500/950/1000 | 0 of 30 | 93.7 W | server exited, does not fit |

The five fit arms are light. Each one loads the model, answers 16 tokens and stops, so the
governor never leaves the 1000 MHz base. The two heavy arms are the envelope.

- `run1` is a prefill ladder: one prompt grown in four 4096-token chunks, 265.4 s of server life.
  Its four measured points are in `arms/run1/ladder.jsonl`, and they fall from 88.44 to 69.62
  tokens per second of prefill between depth 0 and depth 12370. That load produced the 273.6 W
  peak against a 300 W supply.
- `run3` is eight tool-calling requests at a 16384-token context, 108.1 s of server life. That
  load produced the 88.8 C peak, which is 0.2 C under the "stop at once" rule of 89 C.

No arm was stopped by the thermal rule. No reading at or above 87 C held for 10 s, because the
governor pulled the clock back within one or two samples each time. The per-second governor
samples are in each arm's `dpm.txt`. Every arm started below 74 C, so no arm needed a cool-down
wait.

Device health after the last arm: no leftover server process, no TDR, `DpmMode` 1, `CuMode` 40
(`arms/run1/health-after.txt`, `arms/*/dpm-after.txt`).

## What this does not show

1. **The plug reading has an unknown age.** The smart plug reports no measurement timestamp of
   its own. A peak is therefore the highest of 15-second samples, not an instantaneous reading,
   and the scale is the TinyTuya convention for this model and is not calibrated.
2. **n = 1 arm for each context and KV pair.** No pair was repeated.
3. **`run1` did not have the device to itself.** Another trial on the same unit ran two
   3-second probes at about 17:55Z and 17:57Z, inside `run1`. The effect on the four ladder
   points is not quantified, so read them as a lower bound. The fit verdict of `run1` is not
   affected, because residency is read while the server holds its buffers.
4. **The fit verdicts hold for this model file and this server build.** A different quantization
   or a different llama.cpp build allocates differently.
5. **The envelope is not a limit.** It is what these two loads drew. A longer prefill than 265 s
   at this ceiling is not covered by it.
6. **No prefill or decode rate here is a projection.** The projections built on `ladder.jsonl`
   live with the report on the development PC, outside this directory, and no fact cites them.
7. **This directory establishes no tool-calling claim.** `arms/run3/result.json` holds the eight
   graded answers of that arm, and they are kept only because `run3` is one of the two envelope
   arms.
8. **The carve-out is not the normal state of the lab.** 12288 MiB of UMA leaves Windows
   3816 MiB, which games on this unit do not tolerate. The resting state is 8192 MiB.

## Files

| Path | What it is |
|---|---|
| `summary.json` | every arm, collected by the development PC: the result, the supervisor fields, both `vram` reads and the arm metadata |
| `arms/<arm>/supervisor.txt` | the lab-side transcript of one arm: pins, gates, both `vram` reads, the server log and the client output |
| `arms/<arm>/meta.json` | the arm as the development PC drove it, with the kit hashes and the plug peak |
| `arms/<arm>/vram-before.txt`, `vram-alive.txt` | `bc250kmd_cli vram`, before the server and while it holds its buffers |
| `arms/<arm>/tctl-probes.txt` | the fail-closed thermal probe, elapsed seconds and Tctl |
| `arms/<arm>/dpm.txt` | the governor sampled once a second |
| `arms/<arm>/plug.jsonl` | the smart-plug samples, with the raw DPS values |
| `arms/<arm>/client.out`, `result.json` | what the client asked and what it got |
| `arms/run1/ladder.jsonl` | the four prefill steps of `run1`, as the client wrote them |
| `arms/run2f/server.err` | the refusal of 196608 tokens with q8_0 K and V |
| `arms/<arm>/health-*.txt`, `dpm-before.txt`, `dpm-after.txt` | driver health and clock policy around each arm |
| `kit/perf-arm.ps1` | the lab-side supervisor of one arm |
| `kit/client-fit.ps1`, `client-ladder.ps1` | the two clients |
| `kit/set-ceiling-2000.ps1` | the step that set `DpmMaxMHz` to 2000 before the session |
| `sha256.txt` | the hash of every file here |

## How to repeat

1. Set the clock ceiling with `kit/set-ceiling-2000.ps1` and restart the lab.
2. Set the UMA carve-out to 12288 MiB, and confirm the segment-0 limit in a `vram` read.
3. Stage the model and the upstream server on the lab.
4. Run one arm for each context and KV pair, under `kit/perf-arm.ps1`.
5. Read `vram-alive.txt` of each arm, and compare it with the segment-0 limit.
6. Put the carve-out back to 8192 MiB before any game trial.
