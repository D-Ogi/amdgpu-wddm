# Provenance

RapidCheck, a property-based test library for C++, from https://github.com/emil-e/rapidcheck at
commit `2c3c4365aca21ef4e612768fdc95b1ce8b39a651` on branch `master`, taken 2026-11-26. Two
directories and the licence file are imported, unmodified: `include/`, `src/` and `LICENSE.md`.

The library generates test cases from a property instead of a fixed matrix, and it shrinks a failing
case to a small one. `driver/contract/test/record-property-test.cpp` states the rules of the shared
surface records as properties, and `driver/kmd/test/record-flip-state-property.cpp` states the rules
of the flip state machine as a command sequence.

| Directory | Files | Tree object at that commit |
|---|---|---|
| `include/` | 123 | `fa0e5efe1ea3315dc9b3ef9c2750e0147a8ce17b` |
| `src/` | 43 | `1c01e453d3ffddec2d9ac3d1c9243e451c8ead04` |
| `LICENSE.md` | 1 | `07c1dad7ebf60ffd72d5a1bc9f574aeb9f5f283c` (blob) |

A tree object names every file under it and every byte of each file, so these three rows pin all 167
imported files. Taken with `git archive`, which writes the committed bytes, and with the line-ending
conversion turned off, because the global `core.autocrlf` of this host is `true` and a first attempt
without those two settings produced CRLF in all 167 files:

```
git -C <BC250_ROOT>\ref\rapidcheck -c core.autocrlf=false -c core.eol=lf archive --format=tar \
    2c3c4365aca21ef4e612768fdc95b1ce8b39a651 include src LICENSE.md | tar -x -C third_party\rapidcheck
```

Verified after the import, in the direction that proves the bytes rather than assumes them: the git
blob hash of each imported file against the blob of the same path at that commit. 167 files compared,
0 differences, 0 files with a CR byte. `LICENSE.md` is 1297 bytes here and 1297 bytes upstream.

```
git -C <BC250_ROOT>\ref\rapidcheck ls-tree -r 2c3c4365aca21ef4e612768fdc95b1ce8b39a651:include
git hash-object -- third_party\rapidcheck\include\rapidcheck\<path>
```

LF line endings, pinned in `.gitattributes`, so the hashes hold on every checkout. The upstream
`.hpp` and `.cpp` extensions are not in the general rules of that file, so the directory has a rule
of its own.

## What is not imported, and why

`test/`, `examples/`, `doc/`, `extras/`, `ext/` and the build files of the upstream repository. The
library is built from `src/` by `driver/contract/test/run_record_properties.ps1` with the compiler
and the flags of this repository's host tests, so upstream CMake is not used. `ext/` is a submodule
of the test framework that upstream runs its own tests with, and no file of `include/` or `src/`
reaches it: the only header outside `rapidcheck/` that they name is `cxxabi.h`, which
`src/detail/Platform.cpp` includes under `#ifndef _MSC_VER` and which clang-cl does not take.

## Our own changes

None, and none belong here. The library is imported whole so that a newer upstream commit drops in
by one `git archive`. Everything of ours is in `driver/contract/test/` and `driver/kmd/test/`.

The sources compile with `/W3` instead of the `/W4 /WX` of our own host tests, and with
`-Wno-deprecated-declarations`, because the library is C++11 of 2015 and its warnings are not ours to
fix. Our test sources keep `/W4 /WX`.

## Licence

BSD 2-Clause, "Copyright (c) 2014-2015, Emil Eriksson", the whole text in `LICENSE.md` as upstream
carries it. The two conditions are the copyright notice in source form and in binary form. There is
no advertising clause and no endorsement clause. The library is linked into host test binaries that
this project does not ship, and `LICENSE.md` travels with the sources in this directory.
