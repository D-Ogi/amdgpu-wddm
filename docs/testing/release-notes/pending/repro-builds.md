# Pending release-notes lines: each file of the package says where it comes from

This file is not a release. It holds the tester-facing lines for the next tester release notes. The release
step copies the lines into `docs/testing/release-notes/<version>-tester.N.md` and deletes this file.

## Changed

- The `manifest.json` of the package now names, for every file it installs, the exact revision of the source
  code the file was built from and the build recipe that made it. Where a second build of that revision gives
  the same file byte for byte, the manifest says so. Where it does not, the manifest says why in one sentence,
  so a support report can state what the installed file is without a guess.

- The build recipes of this project now make the same bytes from the same source code, in any build folder.
  Before this version the name of the folder a file was built in reached the file itself, so two builds of one
  revision gave two different files and the hash of a file named a build moment and not the code inside it.

- Many files of this package therefore have new hashes against the last release, although the code inside them
  did not change. The files are a little smaller, because each built-in file name is now short.

## Known behaviour

- The catalog file of the kernel driver (`bc250kmd.cat`) is still different in every build, because every
  build signs a new one. The driver itself (`bc250kmd.sys`) and its `.inf` are the same bytes.
