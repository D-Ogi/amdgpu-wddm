# Matched Linux piglit source

PROVENANCE: https://gitlab.freedesktop.org/mesa/piglit, MIT.

The archive for Linux is prepared from0cc014230c0701d7a61bf240009ddd0711462d4e,
the same piglit source revision as the Windows quick package. Archive identity
is in linux-piglit-source.json. Workspace artifact:
`scratch/m12/linux-reference/piglit-0cc01423/source.tar.gz`.

archive_reference.py forces LF Git export and checks every archive file against
its Git blob hash, plus executable and symlink modes. The piglit archive passes
all10035 blob checks, including34 executable files. This is source preparation;
it has not been built or executed on Linux. Its dependencies and the matching
Mesa/RADV/Zink build remain part of L37, as do the full exact quick case list and
comparison through compare_piglit.py. A source archive is not runtime parity.

Reproduce from the workspace root:

    python bc250-win/experiments/E33-m12-applications/archive_reference.py scratch/m12/piglit-src 0cc014230c0701d7a61bf240009ddd0711462d4e scratch/m12/linux-reference/piglit-recheck

Use a new output directory. The verifier rejects omitted/changed files, including
Git export substitutions; inspect those attributes if a different upstream tree
fails. No reference program is executed by preparing this archive.
