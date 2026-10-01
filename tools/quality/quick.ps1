param([Parameter(Mandatory)][string]$Workspace,[Parameter(Mandatory)][string]$Out,[string]$RepoRoot="")
$ErrorActionPreference='Stop'
$repo=if($RepoRoot){$RepoRoot}else{Join-Path $Workspace 'bc250-win'}
$icd=if($env:BC250_RADV_SOURCE){$env:BC250_RADV_SOURCE}else{Join-Path $Workspace 'scratch\m12\mesa-current-src'}
$radvBuild=if($env:BC250_RADV_BUILD){$env:BC250_RADV_BUILD}else{Join-Path $Workspace 'scratch\m12\mesa-current-build'}
$umdSource=if($env:BC250_UMD_SOURCE){$env:BC250_UMD_SOURCE}else{Join-Path $Workspace 'scratch\m13-native-zink-src'}
$umdBuild=if($env:BC250_UMD_BUILD){$env:BC250_UMD_BUILD}else{Join-Path $Workspace 'scratch\m13-native-zink-build'}
$clock=[Diagnostics.Stopwatch]::StartNew()
New-Item -ItemType Directory -Force $Out | Out-Null
$results=@()
function Check([string]$name,[scriptblock]$action) {
    $watch=[Diagnostics.Stopwatch]::StartNew()
    & $action *> (Join-Path $Out "$name.log")
    if($LASTEXITCODE -ne 0){throw "$name failed; see $Out\$name.log"}
    $script:results+=@{name=$name;status='PASS';seconds=$watch.Elapsed.TotalSeconds}
    Write-Host "$name PASS $([Math]::Round($watch.Elapsed.TotalSeconds,2))s"
}
try {
 Check 'kmd-commands' { & pwsh -NoProfile -File "$repo\driver\kmd\build.ps1" -Kits "$Workspace\toolchain\nuget" -Out "$Out\kmd" -ExportCommandsOnly }
 Check 'kmd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$Out\kmd\compile_commands.json" --match '/driver/(kmd|shim)/' --out "$Out\kmd-contract" }
 Check 'radv-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$radvBuild\compile_commands.json" --out "$Out\radv-contract" }
 Check 'umd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$umdBuild\compile_commands.json" --match '/src/gallium/(frontends/d3d10umd|targets/d3d10umd|drivers/zink)/' --out "$Out\umd-contract" }
 Check 'vsync-vector' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vsync_vector.ps1" -Root $Workspace -Out "$Out\vsync-vector" }
 Check 'ih-consume' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_ih_consume.ps1" -Root $Workspace -Out "$Out\ih-consume" }
 Check 'dpm' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_dpm.ps1" -Out "$Out\dpm" -Kits "$Workspace\toolchain\nuget" }
 Check 'clock-policy' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_clock.ps1" -Root $Workspace -Out "$Out\clock-policy" }
 Check 'smu-native' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_smu_native.ps1" -Root $Workspace -Out "$Out\smu-native" }
 Check 'cu-mode' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_cu_mode.ps1" -Out "$Out\cu-mode" -Kits "$Workspace\toolchain\nuget" }
 Check 'hang-progress' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_hang_progress.ps1" -Root $Workspace -Out "$Out\hang-progress" }
 Check 'paging-queue' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_queue.ps1" -Root $Workspace -Out "$Out\paging-queue" }
 Check 'paging-queue-no-quota' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_queue.ps1" -Root $Workspace -Out "$Out\paging-queue-no-quota" -Mutation '--drop-quota' -ExpectFailure }
 Check 'surface-layout' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dcn_translate.ps1" -Root $Workspace -Out "$Out\surface-layout" -Kits "$Workspace\toolchain\nuget" }
 Check 'vidpn-flip' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip" }
 Check 'blit-plan' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_blit_plan.ps1" -Root $Workspace -Out "$Out\blit-plan" -Kits "$Workspace\toolchain\nuget" }
 Check 'interop-policy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_interop_policy.ps1" -Root $Workspace -Out "$Out\interop-policy" -Kits "$Workspace\toolchain\nuget" }
 Check 'allocation-identity' { & python "$repo\tools\quality\allocation_identity.py" --out "$Out\allocation-identity" }
 Check 'object-index' { & python "$repo\tools\quality\object_index.py" --out "$Out\object-index" }
 Check 'gfx-copy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gfx_copy.ps1" -Root $Workspace -Out "$Out\gfx-copy" -Kits "$Workspace\toolchain\nuget" }
 Check 'gfx-blt' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gfx_blt.ps1" -Root $Workspace -Out "$Out\gfx-blt" -Kits "$Workspace\toolchain\nuget" }
 Check 'blob-abi' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_umd_blob.ps1" -Out "$Out\blob-abi" -Kits "$Workspace\toolchain\nuget" -ProducerRoot $icd }
 Check 'surface-control' { & python "$PSScriptRoot\check_surface.py" --mesa "$umdSource" --out "$Out\surface-control" }
 Check 'kmd-analysis' { & python "$PSScriptRoot\msvc_analysis.py" --database "$Out\kmd\compile_commands.json" --match '/umd_blob.c$' --out "$Out\analysis-kmd" }
 Check 'umd-analysis' { & python "$PSScriptRoot\msvc_analysis.py" --database "$umdBuild\compile_commands.json" --match '/Device.cpp$' --out "$Out\analysis-umd" }
 @{status='PASS';seconds=$clock.Elapsed.TotalSeconds;checks=$results} | ConvertTo-Json -Depth 5 | Set-Content "$Out\result.json"
} catch {
 @{status='FAIL';seconds=$clock.Elapsed.TotalSeconds;checks=$results;error=$_.Exception.Message} | ConvertTo-Json -Depth 5 | Set-Content "$Out\result.json"
 Write-Error $_
 exit 1
}

