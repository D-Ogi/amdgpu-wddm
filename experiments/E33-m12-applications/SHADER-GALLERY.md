# Visible shader correctness gallery

The owner requested visible results while driver tests run. The gallery computes
three512x512 fixed-point escape-time images (Mandelbrot, Julia, Burning Ship) through
the existing E14 Vulkan dispatch/readback path, then independently calculates each
pixel on the CPU. Colors represent GPU-read-back iteration counts. The third panel
marks mismatches in pink; equality is checked over all pixels before display.

Build with build-shader-gallery.cmd from this workspace. Run shader-gallery-compute
with a shader directory containing gallery.spv and a fresh output directory. Run
ShaderGallery.exe with that output directory. Use the lab's normal interactive
token and verify the loaded ICD, as in the evidence worker script. Do not launch
the GUI on the development PC without informing its owner.

Space pauses automatic selection, Left/Right select a result, Escape closes.
The8-second timer changes displayed captures; it never dispatches new GPU work.
The UI says that these are captured results, that presentation uses llvmpipe CPU,
and that formal certification is pending. Single submit/wait times are observations,
not benchmarks. Close the viewer for performance measurements.

M486 records the first run: all786432 pixels match. M485's21release sparse-buffer
CTS passes are shown separately from the gallery's3image comparisons. The progress
line can be updated through progress.txt beside results.json. This visual probe
does not replace CTS, image residency tests or Linux comparisons.
