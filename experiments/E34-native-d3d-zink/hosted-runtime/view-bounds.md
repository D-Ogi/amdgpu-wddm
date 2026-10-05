# Sampler-view bounds candidate

Fable017 identified that the frontend bound 128 views regardless of the screen
cap. Zink advertises 32. This incremental patch follows the existing E34 UMD
patches and applies the per-stage cap to normal binding, surface rotation and
teardown. Non-NULL unsupported slots report E_NOTIMPL before changing bound
state. Drivers advertising 128 retain that range. This does not implement 128
views in Zink or correct NULL samplers.

PROVENANCE: Mesa, MIT.

Local build and all eight fast quality gates pass. Three-file LF patch replay
matches the manifest. Validation and DLL hash are retained in
../../../evidence/windows/2026-09-27-E34-view-bounds-build/validation.json.
No lab deployment or runtime regression result is claimed for this candidate.
Next control: retain a PS texture while clearing GS views, then validate exact
pixels; exercise NULL and unsupported high slots separately.
