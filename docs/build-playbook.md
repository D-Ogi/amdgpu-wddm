# Build a release set

Use `tools/release/build-release-set.ps1` to build the components named by a release
manifest. The default is incremental: reuse eligible results in the same output
directory, build changed or missing components, and rebuild their consumers.
Start with `-Plan` so you can inspect the work before compilation starts.

The [build reference](build.md) covers prerequisites and individual recipes. This
playbook covers a complete set and how to continue it after a failure. Run the
commands from this repository in PowerShell 7.

For a code iteration, run the affected component tests first. Then build the
required components with this workflow. Create an installer package when a
candidate needs installation checks. Packaging after each source edit adds work.
Complete the component tests before packaging.

## 1. Select the inputs and output directory

Set `BC250_ROOT` to the workspace containing the repository, portable tools and
companion source repositories. Choose the manifest for the release you are working
on. Keep its output directory for subsequent attempts.

```powershell
if (-not $env:BC250_ROOT) { throw 'Set BC250_ROOT to your workspace first.' }
$set = @{
    Manifest = 'tools\release\release-tester.38.json'
    Root = $env:BC250_ROOT
    Out = Join-Path $env:BC250_ROOT 'scratch\release-set'
    Sources = 'tools\release\release-sources.json'
}
```

The manifest pins source and recipe revisions, arguments, outputs and host tests.
`Sources` supplies payload inputs that the manifest does not build itself. Update
the relevant pins when changing code. Editing a checkout alone does not change a
manifest's pinned source.

Declare external workspace files and directories in each row's `inputs` with the
`workspace` field. The planner checks their content before reuse and refuses missing
inputs. Do not hide new dependencies behind a recipe's default path.

## 2. Review the plan

```powershell
pwsh -NoProfile -File tools\release\build-release-set.ps1 @set -Plan
if ($LASTEXITCODE -ne 0) { throw 'Resolve the reported plan or gate failure first.' }
```

Inspect the selected builds, retained results and reasons for rebuilding. Check
that a changed producer also selects its consumers. The plan runs preflight checks
and writes `plan.json`. It does not compile the components. Resolve missing tools,
source revisions and disk-budget failures before starting the build.

Inspect `build-context.json` if the plan reports changed or unreadable tools.
The tool reads content hashes of the portable tool directories, detected build
tools, relevant environment inputs and build helpers. These checks take time but
can avoid recompilation. This inventory does not trace every possible file access
of a recipe. Keep tools unchanged while a build runs.

An older result without this context record needs a new build. A changed recipe
commit also requires a build because its helper dependencies can change outside
the recipe directory. Read these reasons in `plan.json` before starting a large set.
Do not edit old receipts to make them qualify for reuse.

For a new output directory, supply a previous set with `-KeepFrom` if one exists:

```powershell
$set.KeepFrom = Join-Path $env:BC250_ROOT 'scratch\previous-release-set'
pwsh -NoProfile -File tools\release\build-release-set.ps1 @set -Plan
```

Eligible files are imported after their hashes are checked. The previous set stays
available. Rows that cannot be reused, and consumers whose inputs change, build in
the new directory. A version change can require rebuilding or restamping. Use the
plan's decision for each row.

## 3. Build and inspect the result

```powershell
pwsh -NoProfile -File tools\release\build-release-set.ps1 @set
if ($LASTEXITCODE -ne 0) { throw 'Build failed; inspect release-report.json and the failed row log.' }
$report = Get-Content (Join-Path $set.Out 'release-report.json') -Raw | ConvertFrom-Json
$report.builds | Select-Object id, status, log_dir
```

Each component's `log_dir` names its attempt logs, test output and result. Use that
path from the report: attempts have unique names. `release-sources.rows.json`
contains the output rows and hashes prepared for registration.

The scheduler checkpoints completed components as they finish. Before replacing a
component's old output, it records that the new attempt is running. An interrupted
attempt cannot qualify as a successful cached build. Previous attempt logs remain
available for diagnosis.

## 4. Continue after a failure

Read the failing row's error and logs, fix its cause, then repeat the same command
with the same output directory. Review `-Plan` again if pins or inputs changed.
Successful eligible components remain reusable. The failed component and affected
consumers build again. Do not delete the output directory merely to retry.

The tool refuses a second scheduler in the same output directory. After a hard
termination, it also checks for workers left by that scheduler. If a worker remains,
the error names its process and receipt. Wait for that worker to finish, then repeat
the command. Check its identity before stopping it and its children yourself.
A completed worker that had not yet been recorded by the scheduler may need to
build again. Old interrupted receipts without worker tracking require inspection
before retrying.

| Need | Command option | Consequence |
| --- | --- | --- |
| Continue the same set | Repeat the default command | Retains eligible completed work and selects affected rows. |
| Start a new directory from an earlier set | `-KeepFrom <previous-output>` | Imports eligible results and builds the rest. |
| Rebuild every component | `-Rebuild` | Ignores reusable results and takes the full build time. Cannot be combined with `-Only` or `-KeepFrom`. |
| Select rows manually for diagnosis | `-Only <row-id>` | Advanced selection. Inspect dependency errors and include required producers. Automatic selection is preferred for a complete set. |
| Reduce concurrent work | `-MaxParallel 1` | Reduces concurrent resource use. The complete set may take longer. |
| Continue independent rows after a failure | `-KeepGoing` | Builds unaffected rows while preserving the failure. The set still fails. |

Use `-Rebuild` for a deliberate clean comparison or when investigating reuse
itself. Delete `KeepFrom` from the parameter table before using it. Increasing
`-MaxParallel` or `-Jobs` does not establish a speedup. Inspect CPU use, disk limits
and measured build times before changing them.

## 5. Register and package the completed set

When every required row and gate passes, register the rows:

```powershell
pwsh -NoProfile -File tools\release\build-release-set.ps1 @set -Register
if ($LASTEXITCODE -ne 0) { throw 'Registration failed; inspect the report.' }
```

This uses the same build selection and writes the repository's
`tools/release/release-sources.json`, including updated D3D11 deployment records
where needed. Review the changes and `register-receipt.json`. Packaging requires
the registered files to be committed and pushed through the project's normal
integration process, with a clean tree.

Run the separate package entry point with the UMD source and build directories
required by its quality gates:

```powershell
pwsh -NoProfile -File tools\release\build-release-set.ps1 -Package -From $set.Out `
    -UmdSource $umdSource -UmdBuild $umdBuild
```

Set `$umdSource` and `$umdBuild` to the checked directories for this release
before running that command. Inspect `package-report.json` and the package gate
logs. If an earlier package round occupies the destination, use the reported
`-SetAside <round-name>` procedure to preserve it before retrying.

For changes limited to release notes in an existing gated package, use
`-RepackNotes -From $set.Out`. This preserves component bytes and runs the repack
checks. Use the full package path when component files or package contents change.

Reuse preserves the build preflight, registration and package gates. A
successful host build or package also does not establish lab acceptance. Run the
required release checks against the final packaged files before publication.
