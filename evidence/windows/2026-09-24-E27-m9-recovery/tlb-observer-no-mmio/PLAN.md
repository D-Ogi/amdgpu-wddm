# M397 - Keep diagnostic MMIO outside invalidation

M396 narrows warm failure to the observer after the request write. Linux v6.18
gmc_v10_0_flush_gpu_tlb performs the request, required dummy read, ACK poll
without intervening RLC/GRBM reads. Remove those extra callback reads for
bootstrap and retirement; retain post-flush RLC tracing and all invalidations.
Host harness models all three callback phases and rejects RLC observation
inside the flush; restoring the old callback is the negative control.
Build WDK and validate package before first-load content and one warm trial.
Hardware causation and successful warm recovery are not assumed.
