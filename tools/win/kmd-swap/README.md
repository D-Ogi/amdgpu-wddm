# kmd-swap - the shared helpers of a lab kit that swaps the KMD image

A lab kit can put a candidate KMD image in place of the installed one and leave the rest of the install as it is.
This is valid only when the candidate compiles the state catalog of the installed package.
These helpers make that rule a gate of every such kit.
A kit copies them into its frozen directory and pins their hashes.

## The rule

The INF of a package writes the state catalog id to `Parameters\DefaultsV2\CatalogId`.
The first start of the package moves `SharedState\StateV2` to a generation of the same catalog.
`DriverStateInit` (`driver/kmd/state_kernel.c`) refuses DriverEntry with `STATUS_REVISION_MISMATCH` (0xC0000365) when the
DefaultsV2 catalog is not the catalog of the image.
The KMD also refuses an active generation of another catalog without the handoff of a package install
(`docs/design/driver-state.md`).
A release KMD keeps both refusals: it never runs on a catalog that it does not know.

So a candidate of another catalog cannot start after a swap of the image only.
The device stays in problem code 10, and the arm measures nothing (A1 s3 of BD-223, 2026-11-07:
candidate FE9BBDB6 compiled catalog 02c7eed4, the installed rc1 had b9c5c1b8).

When the catalogs differ, use one of these two routes:

1. Build the candidate on the installed catalog: a lab branch on the source of the installed KMD with the commits
   under test picked onto it. Example: `lab/bd223-a1-rc1-catalog`, the rc1 KMD source with the BD-223 KMD commits.
2. Install a package of the candidate. The installer moves the store to the new catalog in one transaction.

Do not change `DefaultsV2` or `StateV2` by hand to make a swap start.
That would skip the migration of the user's settings, and the restore of the old image would then fail in the same way.

## Files

| File | Side | Does |
|---|---|---|
| `tools/state/kmd_swap_catalog.py image <sys> --repo <root>` | host | Prints the catalog id that a KMD image compiles: the one known id in its bytes. |
| `tools/state/kmd_swap_catalog.py pin <sys> --catalog <id> --repo <root>` | host | Exit 0 when the image compiles the pinned id. A kit freeze runs it. |
| `tools/state/kmd_swap_catalog.py delta --installed <id> --candidate <id> --repo <root>` | host | Names the settings that differ between two catalogs, for the refusal text. |
| `catalog-precheck.ps1 -Expected <id>` | lab | Read only. Exit 0 when DefaultsV2 and the active StateV2 generation have the catalog `<id>`, 5 when not. |
| `swap-sys.ps1 -New <sys> -Sha <sha256> -Catalog <id>` | lab | The image swap. It runs the same check before any change and refuses a swap without `-Catalog`. `target.py ps` sends one file, so it holds a copy of the check block. The host test fails when the copies differ. |
| `test-catalog-precheck.ps1`, `test-swap-sys.ps1` | host | The host tests, in the `kmd-swap-catalog` gate of `tools/quality/quick.ps1`. |

## Use in a kit

1. At the freeze: `kmd_swap_catalog.py pin <candidate sys> --catalog <id>`, and write `<id>` into the pins file of the kit.
2. At the precheck, before the first change on the lab: `catalog-precheck.ps1 -Expected <pinned id>`.
   A refusal stops the kit with nothing changed.
3. At the swap: `swap-sys.ps1 ... -Catalog <pinned id>`. The check runs again right before the copy.
4. A restore (`swap-sys.ps1 -Restore`) needs no catalog: it puts back the image of the installed package.
