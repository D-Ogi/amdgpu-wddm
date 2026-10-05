# M560: GPU DWM with controlled composition and partial execution trace

Source: bc250-win b75330b, candidate UMD897A38A7/hosted ICD3508416F, KMD0.7.152.1.
Exact package hashes and harness are retained. Unit A, hosted DWM PID12804.
The GDI-only control creates a red320x240 window and half-alpha blue160x120
overlap, with a changing16x16 marker. It calls no Direct3D or Vulkan API.

## Image and execution

Independent KMD scanout dumps and the screen capture match the CPU DWM reference
in8000 interior pixels:1400 red pixels255/0/0 and6600 overlap pixels127/0/128.
There are zero reference/GPU or GPU/screen mismatches. Full images remain private;
ROI metrics and SHA256 hashes are retained. This is a controlled-region comparison,
not a whole-desktop pixel oracle or long stability test.

UMD module samples bind PID12804 to the intended hosted stack. Sampled user-mode
monitored fences progress through submitted=completed545 at Present540. DxgKrnl
ETW independently contains551 paired DMA Start/Stop events owned by DWM contexts,
including549 on its main rendering context. Matching tracks Context Start/Stop
ownership and queue sequence; no unmatched stop or pending start occurs among
these retained DWM DMA pairs. The trace reports10931 lost events overall, so this
is partial evidence, not a complete trace. Full ETL and decoded CSV stay private.
Future acquisition must increase ETW buffers before claiming exhaustive coverage.

## CPU-copy observations and limits

KMD before/after summaries retain594 blits and594 translated sources, with no
increase over the25.646s counter interval. The historical widest blit remains1200
rows; that is not a new full-frame copy in this interval. The existing summary
line truncates before its seed count, so it does not independently cover seeds.

DWM does make image-map requests: the cumulative count reaches687/140227152
logical bytes including final capture activity. Buffer requests and persistent
buffer maps are also present. This aggregate cannot distinguish small GDI-input
uploads from full-frame copies or subsequent persistent writes. No-copy G0 is
therefore NOT accepted. Next instrument map dimensions/direction and persistent
image mappings, audit their callers, and retain a loss-free execution trace.

## Recovery and retention

All six process samples retain PID12804. Main runner exits0, restores baseline
UMD8279AC7F/ICD9C40083C and restarts CPU DWM as5324. The independent watchdog
also verifies baselines. No KMD deployment, OS reboot or baseline promotion.

Only account/machine lines are redacted in the public run transcript. The KMD
summary JSON is an explicitly derived last-summary extraction. Raw KMD logs,
images and ETL remain private with hashes; the other public logs are unchanged.
An early attempt to read the live UMD log hit a sharing violation; process/task
polling and later collection established completion without rerunning the probe.
