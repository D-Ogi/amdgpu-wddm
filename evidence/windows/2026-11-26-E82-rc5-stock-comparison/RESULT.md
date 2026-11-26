# RC5 Time Spy results on BC-250 unit A

| Graphics test | Stock mean FPS | Optimized mean FPS | Change |
| --- | ---: | ---: | ---: |
| GT1 | 35.6496 | 37.1679 | +4.26% |
| GT2 | 29.7244 | 31.1574 | +4.82% |

Each package ran three scored passes on Windows build 26300. All clock samples in the graphics workloads were 1,000 MHz. The GPU had 40 active compute units. The highest scored temperature was 77.0 C. The tests recorded no thermal throttling, display resets, or bugchecks. The frame rate ranges did not overlap. The JSON report gives the package hashes, per-pass results, and normalized workload hashes. The six CSV files are the raw clock traces for the scored passes. [Package comparison](stock-comparison.json)

The optimized package used kernel and Direct3D 12 shell source `719e98044`, DXVK `7b3de842`, and the system Vulkan ICD at `c93144ed77d`. The stock package used the pins in `9767822c5e1b7b490af659b987bacefccd71befa`. The package comparison includes these source differences. A separate test on the same optimized image changed only the three settings. It measured gains of 3.98% in GT1 and 4.43% in GT2 with all three settings enabled. [Same-package comparison](same-package-comparison.json)

The original tester.38 driver, state, application files, and benchmark settings were restored after each package. [After stock](stock-restored-binding.json), [after optimized](optimized-restored-binding.json)
