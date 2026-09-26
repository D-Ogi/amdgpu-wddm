# M320: full1GiB acceptance with native-file output

Unchanged07102 KMD/probe and boot, no restart or timeout change.64KiB positive control passes independent validation before the1GiB run. Large run nativeexit0; four full1GiB GPU readbacks all words match, fences1024/2048/3072/4096; all3residency cycles3(NOTRESIDENT)->1 pass. Bulk restoration6360/6266/6266ms. Cumulativegraphics6595/6595 and paging395467/395467,zero timeouts/refusals/noTDR. The4096GFXjobs are actual copies into a separately sentinel-filled readback buffer, checked against the CPU pattern.

Large-run monitor samples span20:30:29..20:34:08, about219s including polling boundaries, within the unchanged300s probe deadline. Previous M319 .NET pipe-output run timed out after300s with2457observed fences. Native-file output restores the M257 transport; this sequential comparison supports a harness-overhead explanation, but does not isolate every run-order/scheduling effect. No GPU-performance speedup is claimed from changing output transport.

Source/validator unchanged from M257; native exits and all invariants independently checked in validation.json. Results establish this1GiB residency/content path on current07102, not physical-PFN relocation, shader-cache policy, arbitrary alias shapes or full M9. Raw logs decoded fromUTF16 where applicable; no semantic edits.
