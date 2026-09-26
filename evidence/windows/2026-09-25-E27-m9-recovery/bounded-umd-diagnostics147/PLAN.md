# Bounded UMD diagnostics on147

Hypothesis: removing routine synchronous logging lowers DWM CPU frame cost.
Cache147 ETW independently resolves DebugPrintf in1584/3195 active DWM CPU samples;
main4336 dominates, unlike earlier shader-worker stalls. Preserve E0ACCC renderer.
Change only Debug.cpp: verbose tracing opt-in BC250_UMD_VERBOSE=1; retain SetError,
assertion/HRESULT diagnostics, renderer identity and existing sampled frame metrics.
OutputDebugString only with an attached debugger. No cache/allocation/render change.
Build then pair with unchanged147, distinct DLL identity, shared/pixel controls and
same Winlogon cursor ETW. Do not attribute primary read-copy costs to logging.
