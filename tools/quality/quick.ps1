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
 # The documentation style ratchet of docs/style.md: a document in tools\quality\doclint_baseline.txt
 # must not get worse, and a document outside it must be clean. It never asks for a rewrite.
 Check 'doclint' { & python "$repo\tools\quality\doclint.py" --root $repo --out "$Out\doclint" }
 # The README Status maps: docs/status must be what tools/docs/status_map.py writes from the driver source
 # and the WDK headers. With the kits it also reads the slot lists from the headers again.
 Check 'status-map' { & python "$repo\tools\docs\status_map.py" --kits "$Workspace\toolchain\nuget" --check; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\docs" -p 'test_*.py' } }
 Check 'ledger' { & python -m unittest discover -s "$repo\tools\win\ledger" }
 # Windows PowerShell 5.1 for the app-route test, because that is the shell its ops scripts run in on the lab.
 Check 'app-route-lib' { & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\win\app-route\ops\tests\test-approute-lib.ps1" -Out "$Out\app-route-lib" }
 Check 'lab-runner-etw' { & pwsh -NoProfile -File "$repo\tools\win\lab-runner\etw\host-checks.ps1" }
 # The present-mode analyser of M15.14. Its fixtures are a kept excerpt of the composed baseline and dumper
 # texts the test writes itself; no .etl and no lab are needed. BC250_ETW_TEST_ETL adds the xperf path.
 Check 'present-mode' { & python "$repo\tools\win\etw\etw-present-mode-test.py" }
 Check 'frameloop-host' { & pwsh -NoProfile -File "$repo\tools\win\frameloop\lab\host-checks.ps1" }
 Check 'register-generators' { & python -m unittest discover -s "$repo\tools\regcalc"; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\diagusb" }; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\win\bc250rd" } }
 Check 'quality-controls' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\quality-controls").FullName; & python -m unittest discover -s "$repo\tools\quality" }
 Check 'kd-dump-triage' { & pwsh -NoProfile -File "$repo\tools\win\kd\analyze-kernel-dump.ps1" -SelfTest }
 Check 'smartplug' { & python "$repo\tools\win\smartplug\selftest.py" }
 Check 'hostwatch' { & pwsh -NoProfile -File "$repo\tools\win\hostwatch\selftest.ps1" -Out "$Out\hostwatch" }
 # The emergency listener runs under Windows PowerShell 5.1 on the lab, so its parser is the one that must accept it.
 Check 'lab-emerg' { & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\win\lab-emerg\parse-check.ps1" }
 Check 'kmd-deploy' { $env:BC250_ROOT=$Workspace; $env:BC250_KMD_DEPLOY_WORK=(New-Item -ItemType Directory -Force "$Out\kmd-deploy").FullName; & python "$repo\tools\win\kmd-deploy\check-offline.py" --quick }
 Check 'lab-runner' { & python -m unittest discover -s "$repo\tools\win\lab-runner" }
 Check 'gpu-timeline' { & python -m unittest discover -s "$repo\tools\win\gpu-timeline" -p 'test_*.py' }
 Check 'gui-trials' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\gui-trials").FullName; & python -m unittest discover -s "$repo\tools\win\gui-trials" -p 'test_run_trial.py'; if($LASTEXITCODE -eq 0){ & pwsh -NoProfile -File "$repo\tools\win\gui-trials\validate-T2.ps1" } }
 Check 'conformance-shaders' { & pwsh -NoProfile -File "$repo\tools\win\conformance-clients\check-shaders.ps1" -Kits "$Workspace\toolchain\nuget" -Out "$Out\conformance-shaders" }
 # -CompileOnly, not -ExportCommandsOnly: the same recipe wrote compile_commands.json for the two contract gates
 # below and compiled nothing, so a C error in driver\kmd or driver\shim passed this whole list and was found only
 # when somebody built a package by hand. -CompileOnly writes the same file and then runs the three cl.exe calls,
 # the link and the stack budget, with no quality gates of its own (this is the list it would call).
 Check 'kmd-compile' { & pwsh -NoProfile -File "$repo\driver\kmd\build.ps1" -Kits "$Workspace\toolchain\nuget" -Out "$Out\kmd" -CompileOnly }
 Check 'kmd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$Out\kmd\compile_commands.json" --match '/driver/(kmd|shim)/' --out "$Out\kmd-contract" }
 Check 'guardlog-width' { & python "$repo\tools\quality\guardlog_width.py" --kmd "$repo\driver\kmd" --baseline "$repo\tools\quality\guardlog_width_baseline.txt" --out "$Out\guardlog-width" }
 # The provenance gate of the release payload (tools/release/provenance.py, run by build-release.ps1): its self-test on
 # throwaway repositories. The gate itself needs the payload files of a workspace, so the release build runs it.
 Check 'release-provenance' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\release-provenance").FullName; & python -m unittest discover -s "$repo\tools\release" -p 'test_provenance.py' }
 # The driver package's own AddReg against the release defaults table (BD-091): every setting the INF writes is the
 # released value and carries NOCLOBBER, so no install outside our installer closes a gate while another stays open.
 Check 'inf-gates' { & python "$repo\tools\quality\inf_gates.py" --inf "$repo\driver\kmd\bc250kmd.inf" --defaults "$repo\tools\release\installer\registry-defaults.json" --out "$Out\inf-gates" }
 Check 'radv-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$radvBuild\compile_commands.json" --out "$Out\radv-contract" }
 Check 'umd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$umdBuild\compile_commands.json" --match '/src/gallium/(frontends/d3d10umd|targets/d3d10umd|drivers/zink)/' --out "$Out\umd-contract" }
 Check 'vsync-vector' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vsync_vector.ps1" -Root $Workspace -Out "$Out\vsync-vector" }
 Check 'ih-consume' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_ih_consume.ps1" -Root $Workspace -Out "$Out\ih-consume" }
 Check 'dpm' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_dpm.ps1" -Out "$Out\dpm" -Kits "$Workspace\toolchain\nuget" }
 Check 'cpu' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_cpu.ps1" -Out "$Out\cpu" -Kits "$Workspace\toolchain\nuget" }
 Check 'clock-policy' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_clock.ps1" -Root $Workspace -Out "$Out\clock-policy" }
 Check 'smu-native' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_smu_native.ps1" -Root $Workspace -Out "$Out\smu-native" }
 Check 'cu-mode' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_cu_mode.ps1" -Out "$Out\cu-mode" -Kits "$Workspace\toolchain\nuget" }
 Check 'hang-progress' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_hang_progress.ps1" -Root $Workspace -Out "$Out\hang-progress" }
 Check 'ring-gap' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_ring_gap.ps1" -Root $Workspace -Out "$Out\ring-gap" }
 Check 'paging-queue' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_queue.ps1" -Root $Workspace -Out "$Out\paging-queue" }
 Check 'paging-queue-no-quota' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_queue.ps1" -Root $Workspace -Out "$Out\paging-queue-no-quota" -Mutation '--drop-quota' -ExpectFailure }
 Check 'surface-layout' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dcn_translate.ps1" -Root $Workspace -Out "$Out\surface-layout" -Kits "$Workspace\toolchain\nuget" }
 Check 'gdi-admission' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gdi_admission.ps1" -Root $Workspace -Out "$Out\gdi-admission" -Kits "$Workspace\toolchain\nuget" }
 Check 'display-modes' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_display_modes.ps1" -Root $Workspace -Out "$Out\display-modes" -Kits "$Workspace\toolchain\nuget" }
 Check 'vidpn-flip' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip" }
 Check 'scanout-admit' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_scanout_admit.ps1" -Root $Workspace -Out "$Out\scanout-admit" -Kits "$Workspace\toolchain\nuget" }
 Check 'dpaudio' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dpaudio.ps1" -Root $Workspace -Out "$Out\dpaudio" -Kits "$Workspace\toolchain\nuget" }
 Check 'modeset' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_modeset.ps1" -Root $Workspace -Out "$Out\modeset" -Kits "$Workspace\toolchain\nuget" }
 Check 'blit-plan' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_blit_plan.ps1" -Root $Workspace -Out "$Out\blit-plan" -Kits "$Workspace\toolchain\nuget" }
 Check 'gpu-clock' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gpu_clock.ps1" -Root $Workspace -Out "$Out\gpu-clock" -Kits "$Workspace\toolchain\nuget" }
 # The per-application graphics settings (docs/design/per-app-graphics-settings.md): the kernel-mode driver's
 # ReportAmdDriverVersion number scheme and decision, and the user-mode reader with its precedence and ranges.
 Check 'driver-version' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_driver_version.ps1" -Root $Workspace -Out "$Out\driver-version" -Kits "$Workspace\toolchain\nuget" }
 Check 'umd-app-settings' { $env:BC250_ROOT=$Workspace; & pwsh -NoProfile -File "$repo\tools\build\test-umd-app-settings.ps1" -OutputDir "$Out\umd-app-settings" }
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
 Check 'ddi-error-policy' { & python "$repo\tools\quality\ddi_error_policy.py" --sources "$repo\driver\umd\dxvk" --reference "$Workspace\ref\ddi-display\d3d10umddi.md" }
 Check 'ddi-error-policy-mutants' { & python "$repo\tools\quality\ddi_error_policy_mutants.py" --sources "$repo\driver\umd\dxvk" --reference "$Workspace\ref\ddi-display\d3d10umddi.md" }
 Check 'ddi-table-versions' { & python "$repo\tools\quality\ddi_table_versions.py" --sources "$repo\driver\umd\dxvk" --kits "$Workspace\toolchain\nuget" }
 Check 'gfx-copy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gfx_copy.ps1" -Root $Workspace -Out "$Out\gfx-copy" -Kits "$Workspace\toolchain\nuget" }
 Check 'gfx-blt' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gfx_blt.ps1" -Root $Workspace -Out "$Out\gfx-blt" -Kits "$Workspace\toolchain\nuget" }
 Check 'blob-abi' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_umd_blob.ps1" -Out "$Out\blob-abi" -Kits "$Workspace\toolchain\nuget" -ProducerRoot $icd }
 Check 'surface-control' { & python "$PSScriptRoot\check_surface.py" --mesa "$umdSource" --out "$Out\surface-control" }
 Check 'kmd-analysis' { & python "$PSScriptRoot\msvc_analysis.py" --database "$Out\kmd\compile_commands.json" --match '/umd_blob.c$' --out "$Out\analysis-kmd" }
 Check 'umd-analysis' { & python "$PSScriptRoot\msvc_analysis.py" --database "$umdBuild\compile_commands.json" --match '/Device.cpp$' --out "$Out\analysis-umd" }
 # Host suites (DEFECTS BD-053): the rest of driver\*\test, each building its fixture from this tree into its own
 # -Out. They do not share outputs, so they run side by side. A suite marked Fails is the runner's own negative
 # control: it must fail by a check (a FAIL line in its log), not by a build error, or the control proves nothing.
 $kits="$Workspace\toolchain\nuget"
 $firmware="$Workspace\ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu"
 $suites=@(
  @{n='display-visibility';s='driver\kmd\test\run_display_visibility.ps1';a=@('-Root',$Workspace)}
  @{n='post-display-stop';s='driver\kmd\test\run_post_display_stop.ps1';a=@('-Root',$Workspace)}
  @{n='scanout-geometry';s='driver\kmd\test\run_scanout_geometry.ps1';a=@('-Root',$Workspace)}
  @{n='post-display';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace)}
  @{n='dcn-flip';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace)}
  @{n='dcn-observation';s='driver\kmd\test\run_dcn_observation.ps1';a=@('-Root',$Workspace)}
  @{n='dcn-observe';s='driver\kmd\test\run_dcn_observe.ps1';a=@('-Root',$Workspace)}
  @{n='display-timing';s='driver\kmd\test\run_display_timing.ps1';a=@('-Root',$Workspace)}
  @{n='display-timing-old-describe';s='driver\kmd\test\run_display_timing.ps1';a=@('-Root',$Workspace,'-OldDescribe60');Fails=$true}
  @{n='display-timing-phase-hz';s='driver\kmd\test\run_display_timing.ps1';a=@('-Root',$Workspace,'-AssumePhaseHz');Fails=$true}
  @{n='gart-retained';s='driver\kmd\test\run_gart_retained.ps1';a=@('-Root',$Workspace)}
  @{n='gfx-retained';s='driver\kmd\test\run_gfx_retained.ps1';a=@('-Root',$Workspace)}
  @{n='ih-retained';s='driver\kmd\test\run_ih_retained.ps1';a=@('-Root',$Workspace)}
  @{n='psp-retained';s='driver\kmd\test\run_psp_retained.ps1';a=@('-Root',$Workspace)}
  @{n='psp-retained-dropped';s='driver\kmd\test\run_psp_retained.ps1';a=@('-Root',$Workspace,'-DropRetain');Fails=$true}
  @{n='retained-gtt';s='driver\kmd\test\run_retained_gtt.ps1';a=@('-Root',$Workspace)}
  @{n='retained-power';s='driver\kmd\test\run_retained_power.ps1';a=@('-Root',$Workspace)}
  @{n='power-coordinator';s='driver\kmd\test\run_power_coordinator.ps1';a=@('-Root',$Workspace)}
  @{n='vram-geometry';s='driver\kmd\test\run_vram_geometry.ps1';a=@('-Root',$Workspace)}
  @{n='guard-start';s='driver\kmd\test\run_guard_start.ps1';a=@('-Root',$Workspace)}
  @{n='guard-start-not-durable';s='driver\kmd\test\run_guard_start.ps1';a=@('-Root',$Workspace,'-IgnoreDurability');Fails=$true}
  @{n='start-health';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace)}
  @{n='start-health-no-flush';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-IgnoreFlush');Fails=$true}
  @{n='gfx-pipeline';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly')}
  @{n='vmid-pool';s='driver\kmd\test\run_vmid_pool.ps1';a=@('-Root',$Workspace)}
  @{n='keep-prune';s='driver\kmd\test\run_keep_prune.ps1';a=@('-Root',$Workspace)}
  @{n='keep-prune-ignore-order';s='driver\kmd\test\run_keep_prune.ps1';a=@('-Root',$Workspace,'-IgnoreOrder');Fails=$true}
  @{n='log-rate';s='driver\kmd\test\run_log_rate.ps1';a=@('-Root',$Workspace)}
  @{n='log-rate-no-gap';s='driver\kmd\test\run_log_rate.ps1';a=@('-Root',$Workspace,'-NoGapReset');Fails=$true}
  @{n='hang-recovery';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace)}
  @{n='hang-recovery-no-fence-guard';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-IgnoreFenceGuard');Fails=$true}
  @{n='hang-recovery-any-vmid';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-IgnoreVmidGuard');Fails=$true}
  @{n='hang-recovery-shared-fence';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-SharedLastCompleted');Fails=$true}
  @{n='hang-recovery-gart-backend';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-GartBackend');Fails=$true}
  @{n='hang-recovery-count-refused';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-CountRefusedKills');Fails=$true}
  @{n='hang-recovery-no-backend-switch';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-NoBackendSwitch');Fails=$true}
  @{n='vmid-pool-ignore-retirement';s='driver\kmd\test\run_vmid_pool.ps1';a=@('-Root',$Workspace,'-IgnoreRetirement');Fails=$true}
  @{n='gfx-pipeline-idle-only';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-IdleOnlyPresent');Fails=$true}
  @{n='gfx-pipeline-no-capacity';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-WithoutCapacity');Fails=$true}
  @{n='gfx-pipeline-equal-fence';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-EqualityFence');Fails=$true}
  # -Root as well: this runner puts the compiler's TEMP under it, and a worktree's parent has no scratch\tmp.
  @{n='firmware-metadata';s='driver\kmd\test\run_firmware_metadata.ps1';a=@('-Root',$Workspace,'-Kits',$kits,'-Firmware',$firmware)}
  @{n='firmware-metadata-no-runtime';s='driver\kmd\test\run_firmware_metadata.ps1';a=@('-Root',$Workspace,'-Kits',$kits,'-Firmware',$firmware,'-OmitRuntimeFirmware');Fails=$true}
  @{n='paging-mc';s='driver\kmd\test\run_paging_mc.ps1';a=@('-Kits',$kits)}
  @{n='paging-private';s='driver\kmd\test\run_paging_private.ps1';a=@('-Kits',$kits)}
  @{n='paging-private-slot-reuse';s='driver\kmd\test\run_paging_private.ps1';a=@('-Kits',$kits,'-ReuseFirstQueueSlot');Fails=$true}
  @{n='paging-pt-shadow';s='driver\kmd\test\run_paging_pt_shadow.ps1';a=@('-Kits',$kits)}
  @{n='paging-stream';s='driver\kmd\test\run_paging_stream.ps1';a=@('-Kits',$kits)}
  @{n='paging-window';s='driver\kmd\test\run_paging_window.ps1';a=@('-Kits',$kits)}
  @{n='umd-caps';s='driver\kmd\test\run_umd_caps.ps1';a=@('-Kits',$kits)}
  @{n='shim-replay';s='driver\shim\test\run.ps1';a=@('-Kits',$kits)}
  @{n='shim-gfx';s='driver\shim\test\run_gfx.ps1';a=@('-Kits',$kits)}
  @{n='shim-ih';s='driver\shim\test\run_ih.ps1';a=@('-Kits',$kits)}
  @{n='shim-psp';s='driver\shim\test\run_psp.ps1';a=@('-Kits',$kits,'-Firmware',$firmware)}
  @{n='shim-pte';s='driver\shim\test\run_pte.ps1';a=@('-Kits',$kits)}
  @{n='shim-paging';s='driver\shim\test\run_paging.ps1';a=@('-Kits',$kits)}
  @{n='sdma-copy';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$kits)}
  @{n='sdma-copy-vmid-zero';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$kits,'-ForceIbVmidZero');Fails=$true}
  @{n='sdma-copy-no-vm-flush';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$kits,'-OmitVmFlush');Fails=$true}
  @{n='sdma-faults';s='driver\shim\test\run_sdma_faults.ps1';a=@('-Kits',$kits)}
  @{n='smu-mailbox';s='driver\shim\test\run_smu.ps1';a=@('-Root',$Workspace)}
  @{n='hwmon';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits)}
  @{n='smu-metrics';s='driver\shim\test\run_smu_metrics.ps1';a=@('-Kits',$kits)}
  @{n='contract-caps';s='driver\contract\test\run.ps1';a=@('-Kits',$kits,'-Mesa',"$Workspace\ref\mesa")}
 )
 # Every runner is in this file, and every runner tests the tree it lives in: one that took its sources from
 # -Root\bc250-win tested the main checkout from any worktree, which is how three suites rotted unseen (BD-053).
 Check 'host-suite-coverage' {
  $self=Get-Content -LiteralPath $PSCommandPath -Raw
  $tree=(Resolve-Path -LiteralPath $repo).Path
  $runners=@(Get-ChildItem "$tree\driver\*\test\run*.ps1" | ForEach-Object { [IO.Path]::GetRelativePath($tree,$_.FullName) })
  $missing=@($runners | Where-Object { !$self.Contains($_) })
  $foreign=@($runners | Where-Object { (Get-Content -LiteralPath (Join-Path $tree $_) -Raw) -match 'bc250-win' })
  if($missing){throw "not in tools\quality\quick.ps1: $($missing -join ', ')"}
  if($foreign){throw "take their sources from -Root\bc250-win instead of their own tree: $($foreign -join ', ')"}
  "$($runners.Count) runners, all gated, all self-located"
 }
 $runs=$suites | ForEach-Object -ThrottleLimit ([Math]::Max(2,[Environment]::ProcessorCount/2)) -Parallel {
  $suite=$_; $log=Join-Path $using:Out "$($suite.n).log"; $watch=[Diagnostics.Stopwatch]::StartNew()
  & pwsh -NoProfile -File (Join-Path $using:repo $suite.s) @($suite.a) -Out (Join-Path $using:Out $suite.n) *> $log
  [pscustomobject]@{n=$suite.n;code=$LASTEXITCODE;fails=[bool]$suite.Fails;log=$log;seconds=$watch.Elapsed.TotalSeconds}
 }
 foreach($run in @($runs | Sort-Object n)){
  if($run.fails){
   if($run.code -eq 0){throw "$($run.n) passed, but it is a negative control and must fail; see $($run.log)"}
   if(!(Select-String -LiteralPath $run.log -Pattern 'FAIL' -CaseSensitive -Quiet)){throw "$($run.n) failed without a FAIL line (build or setup?); see $($run.log)"}
  } elseif($run.code -ne 0){throw "$($run.n) failed; see $($run.log)"}
  $results+=@{name=$run.n;status='PASS';seconds=$run.seconds}
  Write-Host "$($run.n) PASS $([Math]::Round($run.seconds,2))s$(if($run.fails){' (negative control failed as it must)'})"
 }
 @{status='PASS';seconds=$clock.Elapsed.TotalSeconds;checks=$results} | ConvertTo-Json -Depth 5 | Set-Content "$Out\result.json"
} catch {
 @{status='FAIL';seconds=$clock.Elapsed.TotalSeconds;checks=$results;error=$_.Exception.Message} | ConvertTo-Json -Depth 5 | Set-Content "$Out\result.json"
 Write-Error $_
 exit 1
}

