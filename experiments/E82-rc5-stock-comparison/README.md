# E82 Time Spy at 1,000 MHz

## Hypothesis

On BC-250 unit A, the optimized RC5 package increases the mean frame rate in both Time Spy graphics tests at 1,000 MHz. Each optimized pass must exceed all three stock passes for the same test.

## Procedure

Use the graphics-only RC5 packages. Build stock from the pins in `9767822c5e1b7b490af659b987bacefccd71befa`. Use the tested RC5 source `719e98044` for the optimized kernel and Direct3D 12 shell. Use stock graphics settings for the control. Enable private tiling, 2 MiB VRAM pages on the small carve-out, and 64 KiB host pages for the optimized package.

Verify the installed driver images before each test. Set the GPU to 1,000 MHz with 40 compute units. Run a warm-up and three scored passes for each package. Start each pass at or below 71 C. Reject a pass if the clock changes, the GPU throttles, the temperature reaches 83 C, or Windows reports a display reset or bugcheck. Verify that the workload inputs agree after the adapter identifier changes between boots. Verify the original tester.38 installation after each package.

## Result

The optimized package passed the criterion in both graphics tests. See the [results and traces](../../evidence/windows/2026-11-26-E82-rc5-stock-comparison/RESULT.md) and [fact M978](../../docs/facts/games.md#m978).
