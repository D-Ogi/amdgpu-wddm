# M486: visible GPU shader correctness gallery

Unit A,2026-09-25T15:23:12Z. Three512x512 images,786432 total pixels, all bit-exact
against independent CPU arithmetic. Zero mismatches. Binary GPU and CPU outputs,
shader SPIR-V/source and the actual loaded-module witness are in capture.zip.

- Mandelbrot FNV1a64:81d8e010ae34ee79; submit/wait1256.4us.
- Julia:34e4ddef4f4a35a2;397.2us.
- Burning Ship:741618dd3172bda6;456.5us.

Each time is one sample; these are not comparative performance results. GPU work
uses Mesa05 candidate17ADD01F/KMD149,1000MHz/VID116. No experimental sparse flag is
needed for these ordinary buffers. The CPU viewer colors measured iteration counts
and displays GPU/reference/difference views, cycling every8seconds without further
GPU dispatch. DWM continues to use llvmpipe CPU rendering.

The visible window and actual output were checked through the monitor screenshot
API. Private full-desktop captures stay in scratch/m12/shader-gallery, outside the
repository. The viewer was moved to the left so the existing right-hand monitor
panel remains visible. User controls: Space pause, arrows select, Escape close.

This is a visual correctness probe, not Khronos or Windows certification.
Sparse buffers passed21release CTS cases separately in M485. Sparse images, full
CTS, late-bound queued PTE copies and same-unit Linux parity are still open.
