# M597: clean committed source for the hosted UMD candidate

Local Mesa branch g0/index-discard-source, commita0ad8af5ea46b90400d947d82d056f3bd58278c5,
starts at upstream05e6c9622e135ac2aeaf56ec70222642627e2162. It records36 modified
build-relevant inputs copied byte-for-byte from the source tree of UMD49AFB21F.
These include the hosted callbacks/import path and the E34 shader, resource
ownership and index DISCARD fixes. Hashes of every copied input are retained.
Non-build documentation and unrelated flattened metadata from the scratch tree
are excluded. Publication branches and the original build tree remain untouched.

A fresh debugoptimized Meson/Ninja build completes all615 steps in a new build
directory. Before/after checks verify a clean committed source tree and unchanged
bytes for all12949 tracked files. The receipt binds commit, source-manifest hash,
recipe, build log, compile commands and build options to DLL5C74BF98. The full
manifest and build log remain private under scratch/g0-hosted/clean-source.
This check covers tracked source stability; it is not a hermetic-toolchain or
bit-reproducible-build claim. Meson fetched its pinned zlib fallback into the
workspace; generated sources and dependencies are covered by the build process,
not retroactively by the tracked-source manifest.

The new DLL is packaged as candidate053 with unchanged hosted ICD3508416F and
controls from the previously passing matrix. State control118 is prepared but
has not run. Old DLL49AFB21F and its build/source tree are preserved. Its prior
runtime and visual acceptance do not transfer automatically to this new DLL.
Require state, texture, sharing, Present and bounded desktop regression on the
exact new artifact before promotion. No lab mutation, KMD change or G0 closure.

PROVENANCE: Mesa (https://gitlab.freedesktop.org/mesa/mesa), MIT.
