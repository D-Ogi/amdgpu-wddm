# Prepares a lab build tree of fix/vm-flush-reuse whose KMD fits the installed tester.32 package.
#
# The branch adds the catalog entry EnableVmFlushEveryJob, so its state catalog identity (sha256 of catalog.json)
# differs from the one the installed INF wrote into Parameters\DefaultsV2\CatalogId. DriverStateInit refuses such a
# KMD (STATUS_REVISION_MISMATCH), so a .sys swap of the branch build would run without its settings.
# This script puts the catalog of -CatalogFrom back (the identity of the installed package) and replaces the one
# read of EnableVmFlushEveryJob in gfx.c with a constant, so that the two lab images differ only in that constant.
#   pwsh lab-tree.ps1 -Tree P:\BC-250\scratch\vmflush\lab -EveryJob 0 [-CatalogFrom 8b30b6f6]
param(
    [Parameter(Mandatory)][string]$Tree,
    [Parameter(Mandatory)][ValidateSet(0, 1)][int]$EveryJob,
    [string]$CatalogFrom = '8b30b6f6',
    [string]$ExpectCatalog = 'f9c680619453d181e3ebba4bf0aefcc4b5dfb5db92915bbf177640808960d2b9'
)
$ErrorActionPreference = 'Stop'
$catalogFiles = @('driver/state/catalog.json', 'driver/state/catalog.generated.h', 'driver/state/defaults.generated.inf',
                  'driver/state/amdgpu-state.bin', 'driver/kmd/bc250kmd.inf', 'tools/release/installer/registry-defaults.json')
& git -C $Tree checkout $CatalogFrom -- @catalogFiles
if ($LASTEXITCODE -ne 0) { throw "git checkout of the catalog from $CatalogFrom failed" }

$gfx = Join-Path $Tree 'driver\kmd\gfx.c'
$text = [IO.File]::ReadAllText($gfx)
$read = 'gfx->VmFlushEveryJob = (GuardReadSetting(L"EnableVmFlushEveryJob", 0) != 0);'
$tail = '; // lab image: constant, the catalog of the installed package'
$value = if ($EveryJob) { 'TRUE' } else { 'FALSE' }
$constant = "gfx->VmFlushEveryJob = $value$tail"
$found = @($read, "gfx->VmFlushEveryJob = TRUE$tail", "gfx->VmFlushEveryJob = FALSE$tail" | Where-Object { $text.Contains($_) })
if ($found.Count -ne 1) { throw "the EnableVmFlushEveryJob read in gfx.c moved ($($found.Count) matches)" }
$text = $text.Replace($found[0], $constant)
[IO.File]::WriteAllText($gfx, $text, [Text.UTF8Encoding]::new($false))

$header = Get-Content -LiteralPath (Join-Path $Tree 'driver\state\catalog.generated.h') -TotalCount 6
$id = ($header | Select-String -Pattern 'AMDGPU_STATE_CATALOG_ID "([0-9a-f]{64})"').Matches[0].Groups[1].Value
if ($id -ne $ExpectCatalog) { throw "catalog identity $id, expected $ExpectCatalog" }
& python (Join-Path $Tree 'tools\state\generate.py') --check
if ($LASTEXITCODE -ne 0) { throw 'generate.py --check failed in the lab tree' }
if (Select-String -LiteralPath $gfx -Pattern 'EnableVmFlushEveryJob"' -Quiet) { throw 'gfx.c still reads EnableVmFlushEveryJob' }
"lab tree $Tree : catalog $id, VmFlushEveryJob $value"
& git -C $Tree status --short
