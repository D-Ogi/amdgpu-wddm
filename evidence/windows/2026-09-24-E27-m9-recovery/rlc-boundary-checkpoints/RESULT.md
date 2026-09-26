# M393 - Candidate122 distinguishes RLC resume and deferred GFX commit

RunEngineStage accepts an optional callback and records before/after RLC resume
and before/after pending GFX visibility completion. Calls and MMIO ordering are
unchanged. Persistence is selected only for unpublished full-WDDM RUN ending at
stage5, at PASSIVE_LEVEL with special APCs enabled. Ordinary/PLAN routes keep
normal locking. The selected route uses the same GfxPagingLock/GartLock order
and unsafe-fast-mutex branch as CP1; release is paired using persistStartup.
CP1 retains its existing labels; RLC uses separate boundary labels. Every
persisted point calls the existing KeepLog-gated GuardLogKeep.

The harness extracts actual RunEngineStage and GpuMemCompleteGfxBootstrap and
runs four existing success/failure scenarios both with and without tracing.
220 checks pass, including callback order and RLC/flush state at boundaries.
An initial negative-control edit failed compilation due /WX constant condition;
that is not accepted as a behavioral negative. Corrected NULL-callback mutation
compiles and returns1 with4 expected assertion failures. All logs are retained.

WDK build and signing pass. Packagecheck25 checks pass,0errors/0warnings/13notes.
Candidate0.7.122.1 SYS SHA256:
D6EB733181B074ABA35B64AD101A9EFDC7294BDE123277DF9C09B17028AD6D33.
Workspace package: scratch/build/bc250kmd-07122/package-umd.
No runtime validation, installation or new reset. Windows121 remains installed
in M391's recovered display-only session. This is diagnostic instrumentation,
not a fix or proof of warm recovery. Next first-load content/control must prove
all boundaries; then one changed warm trial distinguishes RLC resume from
pending visibility completion. Full M9 remains open.
