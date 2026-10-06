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
# A gate whose inputs are not in the repository. It runs whenever they are present, so that nobody has to
# remember it, and records why it did not when they are absent - never silently.
function CheckIf([string]$name,[string]$needs,[scriptblock]$action) {
    if(Test-Path -LiteralPath $needs){ Check $name $action; return }
    $script:results+=@{name=$name;status='SKIP';reason="missing input: $needs"}
    Write-Host "$name SKIP (missing input: $needs)"
}
try {
 Check 'facts' { & python "$repo\tools\facts\gen_facts.py" --root $repo --check; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\facts" } }
 Check 'register-generators' { & python -m unittest discover -s "$repo\tools\regcalc"; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\diagusb" }; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\win\bc250rd" } }
 Check 'quality-controls' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\quality-controls").FullName; & python -m unittest discover -s "$repo\tools\quality" }
 Check 'kd-dump-triage' { & pwsh -NoProfile -File "$repo\tools\win\kd\analyze-kernel-dump.ps1" -SelfTest }
 Check 'smartplug' { & python "$repo\tools\win\smartplug\selftest.py" }
 Check 'conformance-shaders' { & pwsh -NoProfile -File "$repo\tools\win\conformance-clients\check-shaders.ps1" -Kits "$Workspace\toolchain\nuget" -Out "$Out\conformance-shaders" }
 Check 'kmd-commands' { & pwsh -NoProfile -File "$repo\driver\kmd\build.ps1" -Kits "$Workspace\toolchain\nuget" -Out "$Out\kmd" -ExportCommandsOnly }
 Check 'kmd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$Out\kmd\compile_commands.json" --match '/driver/(kmd|shim)/' --out "$Out\kmd-contract" }
 Check 'guardlog-width' { & python "$repo\tools\quality\guardlog_width.py" --kmd "$repo\driver\kmd" --baseline "$repo\tools\quality\guardlog_width_baseline.txt" --out "$Out\guardlog-width" }
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
 Check 'gdi-admission' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gdi_admission.ps1" -Root $Workspace -Out "$Out\gdi-admission" -Kits "$Workspace\toolchain\nuget" }
 Check 'display-modes' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_display_modes.ps1" -Root $Workspace -Out "$Out\display-modes" -Kits "$Workspace\toolchain\nuget" }
 Check 'vidpn-flip' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip" }
 Check 'scanout-admit' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_scanout_admit.ps1" -Root $Workspace -Out "$Out\scanout-admit" -Kits "$Workspace\toolchain\nuget" }
 Check 'blit-plan' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_blit_plan.ps1" -Root $Workspace -Out "$Out\blit-plan" -Kits "$Workspace\toolchain\nuget" }
 Check 'gpu-clock' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gpu_clock.ps1" -Root $Workspace -Out "$Out\gpu-clock" -Kits "$Workspace\toolchain\nuget" }
 Check 'umd-caps' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_umd_caps.ps1" -Out "$Out\umd-caps" -Kits "$Workspace\toolchain\nuget" }
 # The only host coverage of the DXGKQAITYPE_UMDRIVERPRIVATE branch itself: the firmware section, the
 # adapter identity trailer and the M15.14 scan-out caps trailer, all extracted from wddm.c by text. It was
 # outside this list because it reads the AMD firmware blobs, which are not in the repository - and that is
 # how an edit to that branch could break the test while every gate here stayed green. It now runs wherever
 # the blobs are and says so when they are not.
 CheckIf 'firmware-metadata' "$Workspace\ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu" `
   { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_firmware_metadata.ps1" -Root $Workspace -Out "$Out\firmware-metadata" -Kits "$Workspace\toolchain\nuget" }
 Check 'interop-policy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_interop_policy.ps1" -Root $Workspace -Out "$Out\interop-policy" -Kits "$Workspace\toolchain\nuget" }
 # The router and its M15.14 D3D11_1 front. This gate builds the router and its host-test binaries and runs the
 # suites that need nothing from the lab: the route policy, the two table fills with their completeness counts,
 # the DirectFlip rule with one refusal per clause, and the E26R and LB7A decoders. A null entry in the front's
 # published table is a call into address zero inside dwm.exe, so "every slot is filled" belongs in the gate
 # that runs on every change and not only in a gate somebody remembers to run.
 Check 'router-front' {
     $env:BC250_ROOT=$Workspace
     $build=Join-Path $Out 'router-front'
     & pwsh -NoProfile -File "$repo\tools\build\build-umd-router.ps1" -OutputDir $build
     if($LASTEXITCODE -ne 0){ exit 1 }
     foreach($suite in @('policy','front-tables','front-rule','front-record')){
         & "$build\test-router.exe" child $suite $build
         if($LASTEXITCODE -ne 0){ exit 1 }
     }
 }
 # The rest of the router's host gate drives the real hosted UMD, the real CPU UMD and a DXVK shell package,
 # which are not in the repository. Name them with these three variables and the whole gate runs here too;
 # without them the pure suites above are what runs, and this line says so instead of passing quietly.
 $routerHosted=if($env:BC250_ROUTER_HOSTED_UMD){$env:BC250_ROUTER_HOSTED_UMD}else{Join-Path $Out 'set-BC250_ROUTER_HOSTED_UMD-BC250_ROUTER_CPU_UMD-BC250_ROUTER_APP_PACKAGE'}
 CheckIf 'router-stack' $routerHosted {
     $env:BC250_ROOT=$Workspace
     & pwsh -NoProfile -File "$repo\tools\build\test-umd-router.ps1" -HostedUmd $routerHosted `
       -CpuUmd $env:BC250_ROUTER_CPU_UMD -AppPackage $env:BC250_ROUTER_APP_PACKAGE `
       -Build (Join-Path $Out 'router-front') -OutputDir (Join-Path $Out 'router-stack')
 }
 Check 'escape-abi' { & python -m unittest discover -s "$repo\tools\win\bc250kmd_cli" }
 Check 'allocation-identity' { & python "$repo\tools\quality\allocation_identity.py" --out "$Out\allocation-identity" }
 Check 'object-index' { & python "$repo\tools\quality\object_index.py" --out "$Out\object-index" }
 Check 'hang-witness' { & python "$repo\tools\quality\hang_witness.py" --out "$Out\hang-witness" }
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

