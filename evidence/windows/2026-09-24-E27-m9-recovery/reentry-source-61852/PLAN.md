# M385 - Match upstream reentry reference to runtime kernel version

E28 runtime is6.18.52-0-lts, prior local source is6.18 at commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
Acquire immutable upstream6.18.52 PSP, SDMA5, GFX10 and lifecycle/reset sources;
compare relevant functions before another hardware transition. Preserve exact
URLs/hashes and differences. Seek Alpine package recipe/patch provenance;
upstream tag alone is not proof of the distro module's exact source. No lab
reset or register changes. A discovered difference requires a targeted control
before adoption. No working Linux warm reentry is assumed.
