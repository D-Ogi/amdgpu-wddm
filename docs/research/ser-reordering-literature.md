# Literature: ray and shader-invocation reordering for SER on gfx1013

Target: Shader Execution Reordering (SER) in RADV on gfx1013. The API surfaces are
`VK_EXT_ray_tracing_invocation_reorder` and D3D12 Shader Model 6.9. Stage 1 gives hit objects with
a reorder that does nothing. Stage 2 is a reorder in software across waves. This list is the
literature base for stage 2.

Date of the search: 2026-10-23. Research only: nothing in this document was built or run on the
lab. The bibliographic data comes from Crossref, OpenAlex and the title pages of the papers.

Each entry gives the authors, the title, the venue, the year, the DOI or a public link, and how
much of the source was read. The result lines give the measured numbers of the source in short
form. They are not quotations. A number is valid only for the hardware that the source names.

## 1. Contracts and platform facts

- The D3D12 specification allows a reorder that does nothing. SER is required in Shader Model 6.9.
  The cap `D3D12_FEATURE_DATA_D3D12_OPTIONS22.ShaderExecutionReorderingActuallyReorders` tells
  the application whether the device reorders. Source:
  [DirectX-Specs, Raytracing.md](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/Raytracing.md),
  revision 5a4139b.
- The Vulkan extension can be exposed without a reorder. `rayTracingInvocationReorderReorderingHint`
  tells the application whether the implementation can reorder. Source:
  [VK_EXT_ray_tracing_invocation_reorder proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_ray_tracing_invocation_reorder.adoc),
  revision 01aaacd.
- RDNA 2 has `IMAGE_BVH_INTERSECT_RAY` and `IMAGE_BVH64_INTERSECT_RAY`. A box node does four
  ray/box tests and sorts the four children by distance. A triangle node does one ray/triangle
  test. The RDNA 1 ISA guide has no BVH instruction. Source: the AMD RDNA 2 ISA guide, section 8.2.10.
- Mesa enables the BVH instruction on gfx1013 (`has_image_bvh_intersect_ray`, `rt_ip_version`
  RT_1_1 in `src/amd/common/ac_gpu_info.c`, Mesa 05e6c962). The traversal is a shader
  (`src/amd/vulkan/nir/radv_nir_rt_traversal_shader.c`). gfx1013 has no traversal unit and no
  reorder unit.
- RADV ray tracing uses wave32 on GFX10 and later, unless the perftest option `rtwave64` is set
  (`src/amd/vulkan/radv_physical_device.c`).
- At the end of a shader, RADV takes the next shader address of the first active lane for the
  whole wave. Lanes with other addresses wait for later passes. RADV does no sort and no regroup
  across waves (`src/amd/vulkan/nir/radv_nir_lower_call_abi.c`).
- `nir_lower_shader_calls` splits a shader at `trace_ray`, `report_ray_intersection` and
  `execute_callable` into an initial shader and resume shaders. It pushes all live values to a
  stack. RADV uses it in its CPS mode (`src/compiler/nir/nir_lower_shader_calls.c`).

## 2. SER and vendor reorder hardware

**A1.** NVIDIA, "Shader Execution Reordering", whitepaper v1.0, 2022.
[Link](https://d29g4g2dyqv443.cloudfront.net/sites/default/files/akamai/gameworks/ser-whitepaper.pdf).
Read: full text.
- Result: up to 2x in ray tracing parts of real applications on Ada. There is no table per test.
- The whitepaper names the cases where a reorder loses: cheap hit shaders (shadow, ambient
  occlusion), coherent primary rays and a large live state. On older GPUs the reorder does nothing.

**A2.** Joshua Barczak, Holger Gruen, "Intel Arc Graphics Developer Guide for Real-Time Ray
Tracing in Games", Intel, undated web page.
[Link](https://www.intel.com/content/www/us/en/developer/articles/guide/real-time-ray-tracing-in-games.html).
Read: full text.
- The thread sorting unit sorts stack IDs by shader record address. The compiler spills all live
  state at each repack point. The guide gives no numbers.

**A3.** Martin-Karl Lefrançois, Nia Bickford, "Boosting Ray Tracing Performance with Shader
Execution Reordering: Introducing VK_EXT_ray_tracing_invocation_reorder", Khronos blog, 2025.
[Link](https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder).
Read: full text.
- Result: vk_gltf_renderer trace time 38.34 ms to 20.02 ms. The Hello SER sample is 40 % faster
  on an RTX 4090 and 90 % faster on Intel Arc B. RTX 20 and 30 expose the extension without a reorder.

**A4.** Amar Patel, "D3D12 Shader Execution Reordering" and "Shader Model 6.9 retail and more",
DirectX blog, 2026.
[Link 1](https://devblogs.microsoft.com/directx/shader-execution-reordering/),
[link 2](https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/).
Read: full text.
- NVIDIA RTX 40 and later and Intel Arc B support the API and reorder. AMD Radeon RX 9000 supports
  the API and does not reorder.

**A5.** Gábor Galgóczi, "Mitigating warp divergence in GPU optical photon Monte Carlo: an order of
magnitude speedup with Shader Execution Reordering", arXiv 2608.21396, 2026 (preprint).
[Link](https://arxiv.org/abs/2608.21396). Read: full text.
- Method: an OptiX megakernel with a reorder every four steps. A control build compiles the reorder
  call and does not run it.
- Result (RTX 4090): 14.72x less wall time. The launch configuration gives 3.2x and the executed
  reorder gives 5.1x. Active lanes go from 2.64 to 20.38 of 32.

## 3. Ray sorting and coherence

**B1.** Matt Pharr, Craig Kolb, Reid Gershbein, Pat Hanrahan, "Rendering Complex Scenes with
Memory-Coherent Ray Tracing", SIGGRAPH 1997, 101-108.
[doi:10.1145/258734.258791](https://doi.org/10.1145/258734.258791). Read: abstract.
- Ray queues per scene region. Scenes render with 10 % of the scene in memory.

**B2.** Kirill Garanzha, Charles Loop, "Fast Ray Sorting and Breadth-First Packet Traversal for GPU
Ray Tracing", Computer Graphics Forum 29(2), 289-298, 2010.
[doi:10.1111/j.1467-8659.2009.01598.x](https://doi.org/10.1111/j.1467-8659.2009.01598.x).
Read: abstract.
- Result: about 3x faster than a depth-first tracer in one kernel, for soft shadows.

**B3.** Bochang Moon, Yongyoung Byun, Tae-Joon Kim, Pio Claudio, Hye-Sun Kim, Yun-Ji Ban, Seung
Woo Nam, Sung-Eui Yoon, "Cache-Oblivious Ray Reordering", ACM TOG 29(3), article 28, 2010.
[doi:10.1145/1805964.1805972](https://doi.org/10.1145/1805964.1805972). Read: full text.
- Rays sorted along a space-filling curve of an estimated hit point. More than 10x for scenes
  larger than memory.

**B4.** Daniel Meister, Jakub Bokšanský, Michael Guthe, Jiří Bittner, "On Ray Reordering
Techniques for Faster GPU Ray Tracing", I3D 2020.
[doi:10.1145/3384382.3384534](https://doi.org/10.1145/3384382.3384534),
[arXiv 2506.11273](https://arxiv.org/abs/2506.11273). Read: full text.
- Method: nine sort keys, a radix sort, then a data reorder between wavefront phases.
- Result (RTX 2080 Ti): trace 1.4x to 2.0x faster (DirectX), 1.3x to 1.9x (OptiX). The sort cost
  was not recovered. The data reorder is the most expensive step. An earlier CUDA tracer with a
  sorted index buffer was faster overall in all scenes.

**B5.** Christian Eisenacher, Gregory Nichols, Andrew Selle, Brent Burley, "Sorted Deferred Shading
for Production Path Tracing", Computer Graphics Forum 32(4), 125-132, 2013.
[doi:10.1111/cgf.12158](https://doi.org/10.1111/cgf.12158). Read: full text.
- Result (CPU): with sorted hits, 11.2 min for a scene with four texture layers. Without the sort,
  819 min.

**B6.** Brent Burley et al., "The Design and Evolution of Disney's Hyperion Renderer", ACM TOG
37(3), article 33, 2018. [doi:10.1145/3182159](https://doi.org/10.1145/3182159). Read: full text.
- Sorts rays before the traversal and hits before the shading, in batches of up to 33 million rays.

**B7.** Johannes Hanika, Hendrik P. A. Lensch, Alexander Keller, "Two-Level Ray Tracing with
Reordering for Highly Complex Scenes", Ulm University report 2009-11.
[doi:10.18725/oparu-1754](https://doi.org/10.18725/oparu-1754). Read: first page.
- CPU, scenes larger than memory. Low relevance for this target.

**B8.** Timo Aila, Tero Karras, "Architecture Considerations for Tracing Incoherent Rays", HPG
2010, 113-122. [doi:10.2312/EGGH/HPG10/113-122](https://doi.org/10.2312/EGGH/HPG10/113-122).
Read: full text.
- A simulated architecture with treelet queues: up to 90 % less memory traffic in hard scenes.
  Stack traffic and queue traffic stay as the main cost.

## 4. Shader sorting, compaction and path regeneration

**C1.** Jared Hoberock, Victor Lu, Yuntao Jia, John C. Hart, "Stream Compaction for Deferred
Shading", HPG 2009, 173-180. [doi:10.1145/1572769.1572797](https://doi.org/10.1145/1572769.1572797).
Read: full text.
- Result (NVIDIA Tesla architecture): sorted shading is 5 % slower in a scene with three simple
  shaders. It is 38 % to 3.5x faster in the four other scenes.

**C2.** Ingo Wald, "Active Thread Compaction for GPU Path Tracing", HPG 2011, 51-58.
[doi:10.1145/2018323.2018331](https://doi.org/10.1145/2018323.2018331). Read: full text.
- Result (GTX 480): 2.3x fewer active warps, but only 12 % to 16 % faster. The shared-memory
  version was 3x slower because it cut the occupancy. With heavier shading the gain was 80 %.

**C3.** Jan Novák, Vlastimil Havran, Carsten Dachsbacher, "Path Regeneration for Interactive Path
Tracing", Eurographics 2010 short papers, 61-64.
[doi:10.2312/egsh.20101048](https://doi.org/10.2312/egsh.20101048). Read: full text.
- Result (GTX 285): lane use 32 % to 35 % without regeneration. Regeneration gives 1.2x to 2.2x.

**C4.** Attila T. Áfra, Carsten Benthin, Ingo Wald, Jacob Munkberg, "Local Shading Coherence
Extraction for SIMD-Efficient Path Tracing on CPUs", HPG 2016. No DOI in Crossref or OpenAlex.
Author copy on embree.org. Read: full text.
- Method: a stream of 2048 rays per core, 188 bytes per path, a counting sort by material.
- Result (two Xeon E5-2699 v3, AVX2): 90 % SIMD use in shading, 1.2x to 3x faster in total.

**C5.** Blender Cycles, shader sorting in the GPU wavefront integrator. Commits 4b1d315017ef
(Michael Jones, 2022) and 523bbf706554 (Brecht Van Lommel, 2022).
[Link 1](https://lists.blender.org/pipermail/bf-blender-cvs/2022-July/175712.html),
[link 2](https://lists.blender.org/pipermail/bf-blender-cvs/2022-July/175755.html).
Read: commit text.
- Result: up to 15 % less render time on Apple Silicon. On an RTX A6000, two scenes are faster and
  one is not. The partition is off at 300 shaders or more.

## 5. Wavefront and persistent-thread execution

**D1.** Timo Aila, Samuli Laine, "Understanding the Efficiency of Ray Traversal on GPUs", HPG 2009,
145-149. [doi:10.1145/1572769.1572792](https://doi.org/10.1145/1572769.1572792). Read: full text.
- Result (GTX 285): earlier kernels were 1.5x to 2.5x below the simulated bound, mostly because of
  the work distribution. Persistent threads that fetch rays close most of the gap.

**D2.** Samuli Laine, Tero Karras, Timo Aila, "Megakernels Considered Harmful: Wavefront Path
Tracing on GPUs", HPG 2013, 137-143.
[doi:10.1145/2492045.2492060](https://doi.org/10.1145/2492045.2492060). Read: full text.
- Method: one million paths, 212 bytes of state per path in memory, one kernel per material, queues
  in global memory.
- Result (Tesla K20): the wavefront design is 36 % to 221 % faster than the megakernel. Thread use
  goes from 23 % to 53 %.

**D3.** Kshitij Gupta, Jeff A. Stuart, John D. Owens, "A Study of Persistent Threads Style GPU
Programming for GPGPU Workloads", InPar 2012.
[doi:10.1109/InPar.2012.6339596](https://doi.org/10.1109/InPar.2012.6339596). Read: full text.
- Result: up to 10x on micro-kernels. In one synchronization case on a GTX 295 the persistent
  version was almost always slower, because of atomic contention.

**D4.** Matt Pharr, Wenzel Jakob, Greg Humphreys, "Physically Based Rendering", 4th edition,
chapter 15, "Wavefront Rendering on GPUs".
[Link](https://pbr-book.org/4ed/Wavefront_Rendering_on_GPUs). Read: full text.
- A reference design with one work queue per kind of work. The chapter does not sort.

**D5.** Daniel Meister, Paritosh Kulkarni, Aaryaman Vasishta, Takahiro Harada, "HIPRT: A Ray
Tracing Framework in HIP", Proc. ACM CGIT 7(3), 2024.
[doi:10.1145/3675378](https://doi.org/10.1145/3675378). Read: full text.
- AMD traversal in HIP with the hardware box and triangle instructions. The traversal stack is in
  private memory, or in shared memory with a global overflow. HIPRT does not reorder.

**D6.** Rafael Padilla, Kyle Webster, Austin H. Kim, "Megakernel vs Wavefront GPU Path Tracing",
arXiv 2605.27323, 2026 (preprint). [Link](https://arxiv.org/abs/2605.27323). Read: full text.
- Result (RTX 3060 Ti, Vulkan ray queries): wavefront 13.58 ms, megakernel 15.47 ms.

## 6. Ray stream traversal on CPUs

**E1.** Christiaan P. Gribble, Karthik Ramani, "Coherent Ray Tracing via Stream Filtering", IEEE
RT 2008, 59-66. [doi:10.1109/RT.2008.4634622](https://doi.org/10.1109/RT.2008.4634622).
Read: abstract.

**E2.** Rasmus Barringer, Tomas Akenine-Möller, "Dynamic Ray Stream Traversal", ACM TOG 33(4),
2014. [doi:10.1145/2601097.2601222](https://doi.org/10.1145/2601097.2601222). Read: abstract.
- Result (Core i7-4750HQ, AVX): traversal 36 % to 53 % faster.

## 7. Hardware research on SIMT divergence

**F1.** Wilson W. L. Fung, Ivan Sham, George Yuan, Tor M. Aamodt, "Dynamic Warp Formation and
Scheduling for Efficient GPU Control Flow", MICRO 2007, 407-420.
[doi:10.1109/MICRO.2007.30](https://doi.org/10.1109/MICRO.2007.30). Read: abstract.
- Result: 20.7 % faster on average for about 4.7 % more area.

**F2.** Wilson W. L. Fung, Tor M. Aamodt, "Thread Block Compaction for Efficient SIMT Control
Flow", HPCA 2011, 25-36. [doi:10.1109/HPCA.2011.5749714](https://doi.org/10.1109/HPCA.2011.5749714).
Read: abstract.
- Result: 22 % faster than stacks per warp, 17 % faster than F1.

**F3.** Aniruddha S. Vaidya et al., "SIMD Divergence Optimization through Intra-Warp Compaction",
ISCA 2013, 368-379. [doi:10.1145/2485922.2485954](https://doi.org/10.1145/2485922.2485954).
Read: full text.
- Result: execution time 7 % lower on average, 18 % with a better memory system.

**F4.** Mohammadreza Saed, Yuan Hsi Chou, Lufei Liu, Tyler Nowicki, Tor M. Aamodt, "Vulkan-Sim: A
GPU Architecture Simulator for Ray Tracing", MICRO 2022, 263-281.
[doi:10.1109/MICRO56248.2022.00027](https://doi.org/10.1109/MICRO56248.2022.00027).
Read: full text.
- Result: 35 % SIMT efficiency in the RT units on average. Grouping calls inside a warp raised the
  efficiency and cost 6 % of performance, because of 11 % more memory loads.

**F5.** Alpin Dale, "Characterizing Warp Divergence from Pascal to Blackwell", arXiv 2607.23402,
2026 (preprint). [Link](https://arxiv.org/abs/2607.23402). Read: abstract.
- Divergent paths run one after the other, on Pascal to Blackwell. Low relevance: NVIDIA only.

**F6.** Yuan Hsi Chou, Tyler Nowicki, Tor M. Aamodt, "Treelet Prefetching For Ray Tracing", MICRO
2023, 742-755. [doi:10.1145/3613424.3614288](https://doi.org/10.1145/3613424.3614288).
Read: abstract.
- Result: 32.1 % faster on average than the Vulkan-Sim RT unit, at the same power.

**F7.** Yavuz Selim Tozlu, Huiyang Zhou, "CoopRT: Accelerating BVH Traversal for Ray Tracing via
Cooperative Threads", ISCA 2025, 166-179.
[doi:10.1145/3695053.3731118](https://doi.org/10.1145/3695053.3731118). Read: abstract.
- Idle threads of a warp help the busy threads with the traversal. Result in Vulkan-Sim: up to
  5.11x, geometric mean 2.15x.

## 8. Compilers that split shaders at a trace call

**G1.** Steven G. Parker et al., "OptiX: A General Purpose Ray Tracing Engine", ACM TOG 29(4),
2010. [doi:10.1145/1778765.1778803](https://doi.org/10.1145/1778765.1778803). Read: full text.
- The compiler stores the live registers at each trace call and continues in a state machine. The
  scheduler selects one state for the whole SIMT unit by a static priority. It regroups nothing
  across warps.

**G2.** Mesa `nir_lower_shader_calls` and the RADV call ABI: see section 1.

## 9. Sources not used as evidence

- News items and secondary analyses of SER (2022 to 2025). They are not primary sources.
- Imagination, "Introduction to the PowerVR Photon Architecture". Found only on third-party
  hosts. Not read.
