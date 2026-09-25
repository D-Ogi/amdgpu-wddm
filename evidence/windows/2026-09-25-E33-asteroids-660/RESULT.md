# Asteroids Vulkan: complete660-frame capture

Unit A, unchanged KMD151, boot2026-09-25T20:51:07.500Z. Experimental ICD16CDEA79
uses dynamic gather capacity (M494), now exported in E33 mesa05-wddm2.patch.
An independent Git index replays the complete77-file port onto Mesa05e6c962 exactly.
System ICD9C40083C remains the baseline and is restored after each run.

The benchmark retained660 duration-query objects but left the Vulkan pool at256.
AllocateQueries consumes two slots per duration from the fixed pool, and results
were only read at the end. The application now requests benchmarkFrames duration
entries for Vulkan and D3D12. This fixes a concrete benchmark configuration error;
we have not established the complete causal chain of the prior0x116 kernel crash.

Run008 with corrected query capacity completes660 frames and exits0; the old
PowerShell5.1 runner then rejects the unsupported [ulong] alias. Independent parsing
validates all660 CPU and GPU rows, but the image is980x620, not the intended1080x720.
The upstream window RECT treated width/height as right/bottom from origin100,100.
That calculation is corrected; the runner now uses [uint64] and checks the PPM header.

Run009 (package008, fresh capture cache, no stage tracing) completes in29536ms,
exit0. Exactly660 sequential CPU and GPU rows, positive finite timings, every GPU
conversion ticks/frequency agrees to1e-6ms. Capture is1080x720 RGB8; its exact hash
is in result.json. Visual inspection shows a rendered asteroid field and skybox.
This is not an exact-image comparison: same-frame Linux reference remains pending.
Median CPU update/submit/present37.60335ms and GPU interval1.78794ms are diagnostics
from a capture run, not the five-repeat performance acceptance or active-ALU time.

After009: flags15, generation560773206, epoch5,1000MHz/VID116,68.625C. Same generation
before/after; no restart during008/009. Baseline registration restored. Application,
Vulkan engine, candidate ICD and system loader have module witnesses. No other API,
full CTS, Linux parity or whole M12 acceptance is implied by this result.

PROVENANCE: Diligent Apache-2.0; Mesa MIT. Exact source/dependency hashes are in the
package manifest and E33 source manifests. Optional core WSI trace patch is preserved
in E33-gather-capacity evidence and is inactive in009. Raw artifacts unchanged;
frame660-preview.jpg is a resized JPEG derivative of the immutable PPM for inspection.
