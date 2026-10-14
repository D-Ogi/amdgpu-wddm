param([Parameter(Mandatory)][string]$Workspace,[Parameter(Mandatory)][string]$Out,[string]$RepoRoot="",[ValidateSet('Full','KmdPackage')][string]$Profile='Full',[string]$Kits='',
      [switch]$NoCache,[int]$Jobs=[Environment]::ProcessorCount,[switch]$PlanOnly)
$ErrorActionPreference='Stop'
$repo=if($RepoRoot){$RepoRoot}else{Join-Path $Workspace 'bc250-win'}
if (!$Kits) { $Kits = Join-Path $Workspace 'toolchain\nuget' }
# Both profiles mix -Kits and older -Root-only suites. Resolve one header tree
# before running either kind, and retain the selected context in the receipt.
. (Join-Path $repo 'tools\quality\kmd-build-context.ps1')
$context = Resolve-Bc250PackageContext -Repo $repo -Kits $Kits -QualityWorkspace $Workspace
$Workspace = $context.workspace
$Kits = $context.kits
$context.profile = $Profile
$icd=if($env:BC250_RADV_SOURCE){$env:BC250_RADV_SOURCE}else{Join-Path $Workspace 'scratch\m12\mesa-current-src'}
$radvBuild=if($env:BC250_RADV_BUILD){$env:BC250_RADV_BUILD}else{Join-Path $Workspace 'scratch\m12\mesa-current-build'}
$umdSource=if($env:BC250_UMD_SOURCE){$env:BC250_UMD_SOURCE}else{Join-Path $Workspace 'scratch\m13-native-zink-src'}
$umdBuild=if($env:BC250_UMD_BUILD){$env:BC250_UMD_BUILD}else{Join-Path $Workspace 'scratch\m13-native-zink-build'}
$clock=[Diagnostics.Stopwatch]::StartNew()
New-Item -ItemType Directory -Force $Out | Out-Null
$Out=(Resolve-Path -LiteralPath $Out).Path
$results=@()
$gateJobs=@()
function Check([string]$name,[scriptblock]$action) {
    # Dynamic scriptblocks have no original automatic script-path variables.
    # Keep child pwsh calls: -File exit semantics differ from & script.ps1 when
    # a negative control consumes a failing native command and returns normally.
    $text=$action.ToString().Replace('$PSScriptRoot','$qualityRoot').Replace('$PSCommandPath','$quickPath')
    $script:gateJobs+=@{name=$name;action=$text}
}
# A gate whose inputs are not in the repository. It runs whenever they are present, so that nobody has to
# remember it, and records why it did not when they are absent - never silently.
function CheckIf([string]$name,[string]$needs,[scriptblock]$action) {
    if(Test-Path -LiteralPath $needs){ Check $name $action; return }
    $script:results+=@{name=$name;status='SKIP';reason="missing input: $needs"}
    Write-Host "$name SKIP (missing input: $needs)"
}
try {
 # A named early gate makes a regression in link input order fail before builds.
 # quality-controls also discovers the positive and negative package unit controls.
 Check 'x86-callconv' { & pwsh -NoProfile -File "$repo/tools/build/test-x86-callconv.ps1" -OutputDir "$Out/x86-callconv" }
 # The two engine DLLs of one source line (BD-167): each flavour's own pin, and no run against the other's DLL.
 Check 'engine-abi-flavour' { & python -m unittest discover -s "$repo\tools\build" -p 'test_engine_abi.py' }
 Check 'kmd-order' { & python "$repo\tools\quality\test_kmd_reproducible.py" --out "$Out\kmd-order" }
 # One namespace spans all KMD headers, including optional diagnostic commands.
 Check 'escape-commands' { & python "$repo\tools\quality\escape_commands.py" --root $repo --out "$Out\escape-commands"; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\quality" -p 'test_escape_commands.py' } }
 Check 'facts' { & python "$repo\tools\facts\gen_facts.py" --root $repo --check; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\facts" } }
 # Shares, denominators and modelled labels in the facts rows: a percentage must be the share of the
 # counts next to it, and a rate normalised to another clock must say which numbers the model made.
 Check 'claim-shares' { & python "$repo\tools\quality\claim_shares.py" --root $repo --out "$Out\claim-shares" }
 # The documentation style ratchet of docs/style.md: a document in tools\quality\doclint_baseline.txt
 # must not get worse, and a document outside it must be clean. It never asks for a rewrite.
 Check 'doclint' { & python "$repo\tools\quality\doclint.py" --root $repo --out "$Out\doclint" }
 # Status maps and release tool controls run in Full (required by build-release.ps1), not KmdPackage.
 Check 'status-map' { & python "$repo\tools\quality\kmd_status_profile.py" --repo $repo --profile $Profile --kits $Kits --out "$Out\status-map"; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\docs" -p 'test_*.py' } }
 Check 'ledger' { & python -m unittest discover -s "$repo\tools\win\ledger" }
 # Windows PowerShell 5.1 for the app-route test, because that is the shell its ops scripts run in on the lab.
 Check 'app-route-lib' { & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\win\app-route\ops\tests\test-approute-lib.ps1" -Out "$Out\app-route-lib" }
 # Settings apply/restore runs against a fake system. Never change this PC's services, tasks or registry.
 Check 'system-tuning' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\win\system-tuning\test-system-tuning.ps1" -Out "$Out\system-tuning" }
 Check 'system-tuning-installer' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\release\test-system-tuning-installer.ps1" -Out "$Out\system-tuning-installer" }
 # This gate uses real ACL/DPAPI storage below Out, with no service, task or registry changes.
 Check 'system-tuning-journal' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\win\system-tuning\test-system-tuning-journal.ps1" -Out "$Out\system-tuning-journal" }
 Check 'lab-runner-etw' { & pwsh -NoProfile -File "$repo\tools\win\lab-runner\etw\host-checks.ps1" }
 # Windows PowerShell 5.1, the shell game-runtime.ps1 runs in on the lab: the channel before readiness, the intro skip.
 Check 'lab-runner-intro' { & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\win\lab-runner\intro-skip-check.ps1" }
 # The present-mode analyser of M15.14. Its fixtures are a kept excerpt of the composed baseline and dumper
 # texts the test writes itself; no .etl and no lab are needed. BC250_ETW_TEST_ETL adds the xperf path.
 Check 'present-mode' { & python "$repo\tools\win\etw\etw-present-mode-test.py" }
 Check 'frameloop-host' { & pwsh -NoProfile -File "$repo\tools\win\frameloop\lab\host-checks.ps1" }
 Check 'register-generators' { & python -m unittest discover -s "$repo\tools\regcalc"; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\diagusb" }; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\win\bc250rd" } }
 Check 'quality-controls' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\quality-controls").FullName; & python -m unittest discover -s "$repo\tools\quality" }
 Check 'gate-cache-controls' { & python -B -m unittest discover -s "$repo\tools\quality" -p 'test_gate_cache.py' }
 Check 'wininstall-locale' { & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\wininstall\test-locale.ps1" -Out "$Out\wininstall-locale" }
 Check 'kd-dump-triage' { & pwsh -NoProfile -File "$repo\tools\win\kd\analyze-kernel-dump.ps1" -SelfTest }
 Check 'smartplug' { & python "$repo\tools\win\smartplug\selftest.py" }
 Check 'hostwatch' { & pwsh -NoProfile -File "$repo\tools\win\hostwatch\selftest.ps1" -Out "$Out\hostwatch" }
 # The emergency listener runs under Windows PowerShell 5.1 on the lab, so its parser is the one that must accept it.
 Check 'lab-emerg' { & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\win\lab-emerg\parse-check.ps1" }
 Check 'kmd-deploy' { $env:BC250_ROOT=$Workspace; $env:BC250_KMD_DEPLOY_WORK=(New-Item -ItemType Directory -Force "$Out\kmd-deploy").FullName; & python "$repo\tools\win\kmd-deploy\check-offline.py" --quick }
 Check 'lab-runner' { & python -m unittest discover -s "$repo\tools\win\lab-runner" }
 Check 'target-boot' { & python -m unittest discover -s "$repo\tools\win" -p "test_target*.py" }
 Check 'train-validate' { & python -m unittest discover -s "$repo\tools\win\train-validate" }
 Check 'gpu-timeline' { & python -m unittest discover -s "$repo\tools\win\gpu-timeline" -p 'test_*.py' }
 Check 'isa-xml' { & python -m unittest discover -s "$repo\tools\isa" }
 Check 'gui-trials' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\gui-trials").FullName; & python -m unittest discover -s "$repo\tools\win\gui-trials" -p 'test_run_trial.py'; if($LASTEXITCODE -eq 0){ & pwsh -NoProfile -File "$repo\tools\win\gui-trials\validate-T2.ps1" } }
 Check 'conformance-shaders' { & pwsh -NoProfile -File "$repo\tools\win\conformance-clients\check-shaders.ps1" -Kits "$Kits" -Out "$Out\conformance-shaders" }
 # -CompileOnly, not -ExportCommandsOnly: the same recipe wrote compile_commands.json for the two contract gates
 # below and compiled nothing, so a C error in driver\kmd or driver\shim passed this whole list and was found only
 # when somebody built a package by hand. -CompileOnly writes the same file and then runs the three cl.exe calls,
 # the link and the stack budget, with no quality gates of its own (this is the list it would call).
 Check 'kmd-compile' { & pwsh -NoProfile -File "$repo\driver\kmd\build.ps1" -Kits "$Kits" -Out "$Out\kmd" -CompileOnly }
 Check 'kmd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$Out\kmd\compile_commands.json" --match '/driver/(kmd|shim)/' --out "$Out\kmd-contract" }
 Check 'guardlog-width' { & python "$repo\tools\quality\guardlog_width.py" --kmd "$repo\driver\kmd" --baseline "$repo\tools\quality\guardlog_width_baseline.txt" --out "$Out\guardlog-width" }
 # The provenance gate of the release payload (tools/release/provenance.py, run by build-release.ps1): its self-test on
 # throwaway repositories. The gate itself needs the payload files of a workspace, so the release build runs it.
 Check 'release-provenance' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\release-provenance").FullName; & python -m unittest discover -s "$repo\tools\release" -p 'test_provenance.py' }
 # The driver package's own AddReg against the release defaults table (BD-091): every setting the INF writes is the
 # released value and carries NOCLOBBER, so no install outside our installer closes a gate while another stays open.
 Check 'inf-gates' { & python "$repo\tools\quality\inf_gates.py" --inf "$repo\driver\kmd\bc250kmd.inf" --defaults "$repo\tools\release\installer\registry-defaults.json" --out "$Out\inf-gates" }
 # Upgrade defaults and explicit clock choices must survive the same registry writes as an installation.
 Check 'registry-defaults' { & powershell -NoProfile -File "$repo\tools\release\test-registry-defaults.ps1" }
 # BD-077: the installer half of the CU mode setting. The -CuMode switch must reach the registry plan, and
 # docs\testing\INSTALL.md must state the number of compute units of an install that does not use the switch.
 Check 'cu-mode-installer' { & powershell -NoProfile -File "$repo\tools\release\test-cu-mode-installer.ps1" -Repo $repo }
 # BD-122: the consumers of the KMD's start record (start-confirm and the installer's verify), with fixtures in a
 # scratch key of HKCU that it removes again. Windows PowerShell 5.1, the shell a tester's installer runs in.
 Check 'start-record-consumers' { & powershell -NoProfile -File "$repo\tools\release\test-start-record.ps1" }
 # Owner 2026-10-12: the installer's own diagnostics of a start that did not pass (the decision, the readers and
 # the bounds), with the field case as a fixture. Windows PowerShell 5.1, the shell a tester runs.
 Check 'installer-diagnostics' { & powershell -NoProfile -File "$repo\tools\release\test-diagnostics.ps1" }
 # BD-060 and BD-080: the INF restart directive, the resumed install's inputs, the DWM baseline, and the plan of the
 # two logon tasks (who confirms a driver start, and at whose logon). It writes files under Out and registers no
 # task. Until now it ran only inside test-dryrun.ps1, which needs a built package.
 Check 'session-checks' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\release\test-session-checks.ps1" -WorkRoot "$Out\session-checks" }
 # BD-076: the OpenGL ICD registration of the installer. The architecture list of the release, the values of each
 # architecture in it, the ICD file check, the restore plan of the uninstaller and the step of an install that
 # registers fewer architectures than the computer holds. It writes a scratch key under HKCU and a folder under
 # %TEMP% and removes both. Like session-checks above it ran only inside test-dryrun.ps1, which needs a built
 # package, so a change of the registration could reach a release before any gate read it. Windows PowerShell 5.1,
 # the shell a tester's installer runs in. Without -Package it reads no package and checks the sources instead.
 Check 'opengl-icd' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\release\test-opengl-icd.ps1" -Installer "$repo\tools\release\installer" }
 # The ICD's own build tree is not in the repository, and the 2026-10-10 cleanup removed the one this gate had
 # (scratch\m12\REMOVED-2026-10-10.txt). It ran through Check, so every profile that holds it - the KMD package
 # profile included - failed before its package step on a missing input. It is the same case CheckIf exists for:
 # it runs wherever the tree is (BC250_RADV_BUILD names another one) and records why it did not when it is gone.
 CheckIf 'radv-contract' "$radvBuild\compile_commands.json" `
   { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$radvBuild\compile_commands.json" --out "$Out\radv-contract" }
 Check 'umd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$umdBuild\compile_commands.json" --match '/src/gallium/(frontends/d3d10umd|targets/d3d10umd|drivers/zink)/' --out "$Out\umd-contract" }
 Check 'vsync-vector' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vsync_vector.ps1" -Root $Workspace -Out "$Out\vsync-vector" }
 Check 'ih-consume' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_ih_consume.ps1" -Root $Workspace -Out "$Out\ih-consume" }
 Check 'dpm' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_dpm.ps1" -Out "$Out\dpm" -Kits "$Kits" }
 Check 'cpu' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_cpu.ps1" -Out "$Out\cpu" -Kits "$Kits" }
 Check 'clock-policy' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_clock.ps1" -Root $Workspace -Out "$Out\clock-policy" }
 Check 'smu-native' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_smu_native.ps1" -Root $Workspace -Out "$Out\smu-native" }
 Check 'cu-mode' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_cu_mode.ps1" -Out "$Out\cu-mode" -Kits "$Kits" }
 Check 'uma-policy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_uma.ps1" -Root $Workspace -Out "$Out\uma-policy" }
 Check 'uma-request' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_uma_request.ps1" -Root $Workspace -Out "$Out\uma-request" }
 Check 'uma-probe' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_uma_probe.ps1" -Root $Workspace -Out "$Out\uma-probe" }
 Check 'board-memory-identity' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_identity.ps1" -Root $Workspace -Out "$Out\board-memory-identity" }
 Check 'board-memory-identity-qualification' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_identity_qualification.ps1" -Root $Workspace -Out "$Out\board-memory-identity-qualification" }
 Check 'board-memory-detect' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_memory_detect.ps1" -Root $Workspace -Out "$Out\board-memory-detect" }
 Check 'board-memory-transport' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_uma_transport.ps1" -Root $Workspace -Out "$Out\board-memory-transport" }
 Check 'board-memory-service' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_memory_service.ps1" -Root $Workspace -Out "$Out\board-memory-service" }
 Check 'board-memory-store' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_memory_store.ps1" -Root $Workspace -Out "$Out\board-memory-store" }
 Check 'board-memory-write' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_memory_write.ps1" -Root $Workspace -Out "$Out\board-memory-write" }
 Check 'board-provider' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_provider.ps1" -Root $Workspace -Out "$Out\board-provider" }
 Check 'board-provider-admission-control' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_board_provider.ps1" -Root $Workspace -Out "$Out\board-provider-admission-control" -Mutation admission }
 Check 'adapter-owner' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_adapter_owner.ps1" -Root $Workspace -Out "$Out\adapter-owner" }
 Check 'adapter-owner-admission-control' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_adapter_owner.ps1" -Root $Workspace -Out "$Out\adapter-owner-admission-control" -Mutation admission }
 Check 'adapter-owner-cleanup-control' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_adapter_owner.ps1" -Root $Workspace -Out "$Out\adapter-owner-cleanup-control" -Mutation cleanup }
 Check 'adapter-owner-quarantine-control' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_adapter_owner.ps1" -Root $Workspace -Out "$Out\adapter-owner-quarantine-control" -Mutation poison }
 Check 'hang-progress' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_hang_progress.ps1" -Root $Workspace -Out "$Out\hang-progress" }
 Check 'wave-reader' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_wave.ps1" -Kits $Kits -Out "$Out\wave-reader" }
 Check 'wave-access' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_wave_access.ps1" -Kits $Kits -Out "$Out\wave-access" }
 Check 'wave-recovery' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_wave_recovery.ps1" -Kits $Kits -Out "$Out\wave-recovery" }
 # Compile the retained HIP journal with host kernel primitives; the ownership mutation must fail an assertion.
 Check 'hip-journal' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_hip_journal.ps1" -Kits "$Kits" -Out "$Out\hip-journal" }
 Check 'hip-journal-owner-control' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_hip_journal.ps1" -Kits "$Kits" -Out "$Out\hip-journal-owner-control" -OmitProcessCheck -ExpectFailure }
 # BD-114: the private submit watchdog's budget, its progress window, its head stamp and the aborted-fence
 # answer, with the source checks that hold wddm.c to the same decisions (the host test cannot compile it).
 Check 'recovery-protocol' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_recovery_protocol.ps1" -Root $Workspace -Out "$Out\recovery-protocol" }
 Check 'submit-watchdog' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_submit_watchdog.ps1" -Root $Workspace -Out "$Out\submit-watchdog" }
 Check 'ring-gap' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_ring_gap.ps1" -Root $Workspace -Out "$Out\ring-gap" }
 Check 'paging-queue' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_queue.ps1" -Root $Workspace -Out "$Out\paging-queue" }
 Check 'paging-queue-no-quota' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_queue.ps1" -Root $Workspace -Out "$Out\paging-queue-no-quota" -Mutation '--drop-quota' -ExpectFailure }
 # BD-114 7.1: node 1's deadline and timer priced from the old flat 500 ms again must fail by a CHECK.
 Check 'paging-queue-flat-budget' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_queue.ps1" -Root $Workspace -Out "$Out\paging-queue-flat-budget" -Mutation '--flat-paging-budget' -ExpectFailure }
 # BD-085: the same queue harness plus the actual preempt/report pass - the DMA boundary, the completion-before-
 # preemption order, same-fence scheduler replay and the native root gate. It was in no gate and rotted twice
 # (KMD172 renamed the report pass, KMD231 gave it the hang-recovery node state), so it is here now.
 Check 'paging-preemption' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_preemption.ps1" -Root $Workspace -Out "$Out\paging-preemption" }
 Check 'paging-preemption-old-drain' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_preemption.ps1" -Root $Workspace -Out "$Out\paging-preemption-old-drain" -Mutation '--old-drain' -ExpectFailure }
 Check 'paging-preemption-any-root' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_preemption.ps1" -Root $Workspace -Out "$Out\paging-preemption-any-root" -AnyRootNative -ExpectFailure }
 # One control per half of the native root gate: dropping only the 4 KiB alignment check left the whole suite
 # green until the misaligned-root check was added (BD-085 review finding 2).
 Check 'paging-preemption-any-align' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_preemption.ps1" -Root $Workspace -Out "$Out\paging-preemption-any-align" -AnyAlignNative -ExpectFailure }
 Check 'surface-layout' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dcn_translate.ps1" -Root $Workspace -Out "$Out\surface-layout" -Kits "$Kits" }
 Check 'gdi-admission' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gdi_admission.ps1" -Root $Workspace -Out "$Out\gdi-admission" -Kits "$Kits" }
 Check 'display-modes' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_display_modes.ps1" -Root $Workspace -Out "$Out\display-modes" -Kits "$Kits" }
 Check 'vidpn-flip' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip" }
 Check 'scanout-admit' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_scanout_admit.ps1" -Root $Workspace -Out "$Out\scanout-admit" -Kits "$Kits" }
 Check 'dpaudio' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dpaudio.ps1" -Root $Workspace -Out "$Out\dpaudio" -Kits "$Kits" }
 Check 'modeset' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_modeset.ps1" -Root $Workspace -Out "$Out\modeset" -Kits "$Kits" }
 Check 'atom-display' {
  $atomArgs=@('-Root',$Workspace,'-Out',"$Out\atom-display",'-Kits',$Kits)
  if($env:BC250_LINK_TEST_ROM){$atomArgs+=@('-PrivateRom',$env:BC250_LINK_TEST_ROM)}
  & pwsh -NoProfile -File "$repo\driver\kmd\test\run_atom.ps1" @atomArgs
 }
 Check 'link-command-import' { & python -B "$repo\tools\import\extract_link_commands.py" --check }
 Check 'link-register-import' { & python -B "$repo\tools\import\extract_link_regs.py" --check }
 Check 'blit-plan' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_blit_plan.ps1" -Root $Workspace -Out "$Out\blit-plan" -Kits "$Kits" }
 Check 'gpu-clock' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gpu_clock.ps1" -Root $Workspace -Out "$Out\gpu-clock" -Kits "$Kits" }
 # The per-application graphics settings (docs/design/per-app-graphics-settings.md): the kernel-mode driver's
 # ReportAmdDriverVersion number scheme and decision, and the user-mode reader with its precedence and ranges.
 Check 'driver-version' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_driver_version.ps1" -Root $Workspace -Out "$Out\driver-version" -Kits "$Kits" }
 Check 'umd-app-settings' { $env:BC250_ROOT=$Workspace; & pwsh -NoProfile -File "$repo\tools\build\test-umd-app-settings.ps1" -OutputDir "$Out\umd-app-settings" }
 # The encoder MFT's source-against-sweep checks: every deblocking schedule the source admits still has a case
 # in tests\sweep.ps1 that holds it to the inbox decoder. The sweep itself needs a GPU and is not in this list;
 # this gate needs nothing but the files, and it is what catches a shape that loses its case when the default moves.
 Check 'mft-h264-host' { & pwsh -NoProfile -File "$repo\driver\umd\mft-h264\tests\host-checks.ps1" }
 Check 'umd-caps' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_umd_caps.ps1" -Out "$Out\umd-caps" -Kits "$Kits" }
 # The only host coverage of the DXGKQAITYPE_UMDRIVERPRIVATE branch itself: the firmware section, the
 # adapter identity trailer and the M15.14 scan-out caps trailer, all extracted from wddm.c by text. It was
 # outside this list because it reads the AMD firmware blobs, which are not in the repository - and that is
 # how an edit to that branch could break the test while every gate here stayed green. It now runs wherever
 # the blobs are and says so when they are not.
 CheckIf 'firmware-metadata' "$Workspace\ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu" `
   { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_firmware_metadata.ps1" -Root $Workspace -Out "$Out\firmware-metadata" -Kits "$Kits" }
 Check 'interop-policy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_interop_policy.ps1" -Root $Workspace -Out "$Out\interop-policy" -Kits "$Kits" }
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
 Check 'ddi-table-versions' { & python "$repo\tools\quality\ddi_table_versions.py" --sources "$repo\driver\umd\dxvk" --kits "$Kits" }
 Check 'flip-rule-names' { & python "$repo\tools\quality\flip_rule_names.py" --router "$repo\driver\umd\router\front-direct-flip.h" --log "$repo\driver\umd\router\front-flip-log.h" --document "$repo\docs\design\direct-flip-handshake.md" }
 Check 'gfx-copy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gfx_copy.ps1" -Root $Workspace -Out "$Out\gfx-copy" -Kits "$Kits" }
 Check 'gfx-blt' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gfx_blt.ps1" -Root $Workspace -Out "$Out\gfx-blt" -Kits "$Kits" }
 # The blob reader has two halves. The contract half needs nothing outside the repository. The producer half
 # extracts the ICD's own field writes from the ICD source tree, whose input went with the ICD trees on
 # 2026-10-10 (scratch\m12\REMOVED-2026-10-10.txt) and took the whole gate down with it, the same way
 # radv-contract above went down. So they are separate now: the contract half always runs, and the producer half
 # runs wherever the tree is (BC250_RADV_SOURCE names another one) and records why it did not when it is absent.
 Check 'blob-abi' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_umd_blob.ps1" -Out "$Out\blob-abi" -Kits "$Kits" }
 CheckIf 'blob-abi-producer' "$icd\src\amd\vulkan\winsys\wddm2\radv_wddm2_bc250.h" `
   { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_umd_blob.ps1" -Out "$Out\blob-abi-producer" -Kits "$Kits" -ProducerRoot $icd }
 Check 'surface-control' { & python "$PSScriptRoot\check_surface.py" --mesa "$umdSource" --out "$Out\surface-control" }
 Check 'kmd-analysis' { & python "$PSScriptRoot\msvc_analysis.py" --database "$Out\kmd\compile_commands.json" --match '/umd_blob.c$' --out "$Out\analysis-kmd" }
 Check 'umd-analysis' { & python "$PSScriptRoot\msvc_analysis.py" --database "$umdBuild\compile_commands.json" --match '/Device.cpp$' --out "$Out\analysis-umd" }
 # Host suites (DEFECTS BD-053): the rest of driver\*\test, each building its fixture from this tree into its own
 # -Out. They do not share outputs, so they run side by side. A suite marked Fails is the runner's own negative
 # control: it must fail by a check (a FAIL line in its log), not by a build error, or the control proves nothing.
 $firmware="$Workspace\ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu"
 if($env:BC250_LINK_TEST_ROM) {
  Check 'link-active-off' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_link_active_off.ps1" -Root $Workspace -Out "$Out\link-active-off" -Kits $Kits -Rom $env:BC250_LINK_TEST_ROM }
  Check 'link-old-clock-policy' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_link_active_off.ps1" -Root $Workspace -Out "$Out\link-old-clock-policy" -Kits $Kits -Rom $env:BC250_LINK_TEST_ROM -OldClockPolicy -ExpectFailure }
  Check 'link-strict-clock-values' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_link_active_off.ps1" -Root $Workspace -Out "$Out\link-strict-clock-values" -Kits $Kits -Rom $env:BC250_LINK_TEST_ROM -StrictValues -ExpectFailure }
  Check 'link-clock-reinit' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_link_atom.ps1" -Root $Workspace -Out "$Out\link-clock-reinit" -Kits $Kits -Rom $env:BC250_LINK_TEST_ROM }
  Check 'link-clock-old-scope' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_link_atom.ps1" -Root $Workspace -Out "$Out\link-clock-old-scope" -Kits $Kits -Rom $env:BC250_LINK_TEST_ROM -OldScope -ExpectFailure }
 } else {
  $results+=@{name='link-active-off';status='SKIP';reason='Requires an external target ATOM ROM supplied through BC250_LINK_TEST_ROM'}
  Write-Host 'link-active-off SKIP (BC250_LINK_TEST_ROM not supplied)'
  foreach($name in @('link-clock-reinit','link-clock-old-scope')) {
   $results+=@{name=$name;status='SKIP';reason='Requires an external target ATOM ROM supplied through BC250_LINK_TEST_ROM'}
   Write-Host "$name SKIP (BC250_LINK_TEST_ROM not supplied)"
  }
 }
 $suites=@(
  @{n='link-atom';s='driver\kmd\test\run_link_atom.ps1';a=@('-Root',$Workspace,'-Kits',$Kits)}
  @{n='connector-buffer';s='driver\kmd\test\run_connector_buffer.ps1';a=@('-Root',$Workspace,'-Kits',$Kits)}
  @{n='connector-modes';s='driver\kmd\test\run_connector_modes.ps1';a=@('-Root',$Workspace)}
  @{n='connector-link';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits)}
  @{n='connector-link-stale-generation';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitGeneration','-ExpectFailure')}
  @{n='connector-link-open-cleanup';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitOpenCleanup','-ExpectFailure')}
  @{n='connector-link-cold-recovery';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitColdRecovery','-ExpectFailure')}
  @{n='connector-link-first-failure';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitFirstFailure','-ExpectFailure')}
  @{n='connector-link-nested-hdmi';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-NestedHdmiStop','-ExpectFailure')}
  @{n='connector-link-diagnostic-replay';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitDiagnosticReplay','-ExpectFailure')}
  @{n='connector-link-write-requested';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-WriteRequestedValue','-ExpectFailure')}
  @{n='connector-link-recovery-target';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-WrongRecoveryTarget','-ExpectFailure')}
  @{n='dml201';s='driver\shim\test\run_dml201.ps1';a=@('-Kits',$Kits)}
  @{n='dcn201-pipe';s='driver\shim\test\run_dcn201_pipe.ps1';a=@('-Kits',$Kits)}
  @{n='dpaudio-start-debt';s='driver\kmd\test\run_dpaudio_debt.ps1';a=@('-Root',$Workspace,'-Kits',$Kits)}
  @{n='wave-recovery-pending-control';s='driver\kmd\test\run_wave_recovery.ps1';a=@('-Kits',$Kits,'-NoPendingMarker');Fails=$true}
  @{n='wave-recovery-order-control';s='driver\kmd\test\run_wave_recovery.ps1';a=@('-Kits',$Kits,'-ClearBeforeData');Fails=$true}
  @{n='wave-recovery-delete-control';s='driver\kmd\test\run_wave_recovery.ps1';a=@('-Kits',$Kits,'-AdminReadOnly');Fails=$true}
  @{n='display-visibility';s='driver\kmd\test\run_display_visibility.ps1';a=@('-Root',$Workspace)}
  @{n='post-display-stop';s='driver\kmd\test\run_post_display_stop.ps1';a=@('-Root',$Workspace)}
  @{n='scanout-geometry';s='driver\kmd\test\run_scanout_geometry.ps1';a=@('-Root',$Workspace)}
  @{n='post-display';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace)}
  @{n='post-display-old-runtime-order';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace,'-OldRuntimeOrder');Fails=$true}
  @{n='post-display-wrong-saved-surface';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace,'-WrongSavedSurface');Fails=$true}
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
  @{n='psp-retained-false-success';s='driver\kmd\test\run_psp_retained.ps1';a=@('-Root',$Workspace,'-PowerSuccessRefusal');Fails=$true}
  @{n='retained-gtt';s='driver\kmd\test\run_retained_gtt.ps1';a=@('-Root',$Workspace)}
  @{n='retained-power';s='driver\kmd\test\run_retained_power.ps1';a=@('-Root',$Workspace)}
  @{n='power-coordinator';s='driver\kmd\test\run_power_coordinator.ps1';a=@('-Root',$Workspace)}
  @{n='vram-geometry';s='driver\kmd\test\run_vram_geometry.ps1';a=@('-Root',$Workspace)}
  @{n='guard-start';s='driver\kmd\test\run_guard_start.ps1';a=@('-Root',$Workspace)}
  # BD-122: the start record, and the four pre-fix behaviours as its controls, one per requirement.
  @{n='start-record';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace)}
  @{n='stop-record';s='driver\kmd\test\run_stop_record.ps1';a=@('-Root',$Workspace)}
  @{n='start-record-keeplog-gate';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-GateOnKeepLog');Fails=$true}
  @{n='start-record-no-attempt';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-NoAttemptRecord');Fails=$true}
  @{n='start-record-before-cleanup';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-RecordBeforeCleanup');Fails=$true}
  @{n='start-record-any-irql';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-RecordAtAnyIrql');Fails=$true}
  @{n='start-record-log-after-cleanup';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-LogAfterCleanup');Fails=$true}
  @{n='guard-start-not-durable';s='driver\kmd\test\run_guard_start.ps1';a=@('-Root',$Workspace,'-IgnoreDurability');Fails=$true}
  @{n='start-health';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace)}
  @{n='start-health-no-flush';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-IgnoreFlush');Fails=$true}
  # BD-120: the witness re-arms after a recovered engine reset, and after nothing else.
  @{n='start-health-no-witness-reopen';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-NoWitnessReopen');Fails=$true}
  @{n='start-health-reopen-any-close';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-ReopenAnyClose');Fails=$true}
  @{n='start-health-sticky-fault-keeps-mark';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-StickyFaultKeepsMark');Fails=$true}
  @{n='start-health-confirm-new-epoch';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-ConfirmCertifiesNewEpoch');Fails=$true}
  @{n='start-health-mark-over-failed-engines';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-MarkOverFailedEngines');Fails=$true}
  @{n='start-health-refusal-drops-other-node-mark';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-RefusalDropsOtherNodeMark');Fails=$true}
  @{n='start-health-rearm-skips-open-witness';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-RearmSkipsOpenWitness');Fails=$true}
  @{n='start-health-confirm-hung-request';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-ConfirmHungRequest');Fails=$true}
  @{n='gfx-pipeline';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly')}
  @{n='vmid-pool';s='driver\kmd\test\run_vmid_pool.ps1';a=@('-Root',$Workspace)}
  @{n='keep-prune';s='driver\kmd\test\run_keep_prune.ps1';a=@('-Root',$Workspace)}
  @{n='keep-prune-ignore-order';s='driver\kmd\test\run_keep_prune.ps1';a=@('-Root',$Workspace,'-IgnoreOrder');Fails=$true}
  @{n='log-rate';s='driver\kmd\test\run_log_rate.ps1';a=@('-Root',$Workspace)}
  @{n='log-rate-no-gap';s='driver\kmd\test\run_log_rate.ps1';a=@('-Root',$Workspace,'-NoGapReset');Fails=$true}
  @{n='log-rate-fan-block';s='driver\kmd\test\run_log_rate.ps1';a=@('-Root',$Workspace,'-FanBlockGrow');Fails=$true}
  @{n='dpm-log-cadence';s='driver\kmd\test\run_dpm_log_cadence.ps1';a=@('-Root',$Workspace)}
  @{n='dpm-log-cadence-no-idle';s='driver\kmd\test\run_dpm_log_cadence.ps1';a=@('-Root',$Workspace,'-NoIdleCadence');Fails=$true}
  @{n='dpm-log-cadence-60s';s='driver\kmd\test\run_dpm_log_cadence.ps1';a=@('-Root',$Workspace,'-IdleSixtySeconds');Fails=$true}
  @{n='hang-recovery';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace)}
  @{n='hang-recovery-no-fence-guard';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-IgnoreFenceGuard');Fails=$true}
  @{n='hang-recovery-any-vmid';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-IgnoreVmidGuard');Fails=$true}
  @{n='hang-recovery-shared-fence';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-SharedLastCompleted');Fails=$true}
  @{n='hang-recovery-gart-backend';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-GartBackend');Fails=$true}
  @{n='hang-recovery-count-refused';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-CountRefusedKills');Fails=$true}
  @{n='hang-recovery-no-backend-switch';s='driver\kmd\test\run_hang_recovery.ps1';a=@('-Root',$Workspace,'-NoBackendSwitch');Fails=$true}
  @{n='submit-watchdog-CountUnchangedAsActivity';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-CountUnchangedAsActivity');Fails=$true}
  @{n='submit-watchdog-CountPrimeAsActivity';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-CountPrimeAsActivity');Fails=$true}
  @{n='submit-watchdog-CountGapAsActivity';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-CountGapAsActivity');Fails=$true}
  @{n='submit-watchdog-IgnoreAlternatingActivity';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-IgnoreAlternatingActivity');Fails=$true}
  @{n='submit-watchdog-AssumeOsClockAtHead';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-AssumeOsClockAtHead');Fails=$true}
  @{n='submit-watchdog-HeadAgeAtRetirement';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-HeadAgeAtRetirement');Fails=$true}
  @{n='recovery-protocol-DropAdmissionReason';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace, '-DropAdmissionReason');Fails=$true}
  @{n='recovery-protocol-RejectActiveImmediately';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace, '-RejectActiveImmediately');Fails=$true}
  @{n='recovery-protocol-IgnoreDrainDeadline';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace, '-IgnoreDrainDeadline');Fails=$true}
  @{n='recovery-protocol-FaultHealthOnAdmission';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace, '-FaultHealthOnAdmission');Fails=$true}
  @{n='recovery-protocol-AdmitDuringReset';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace, '-AdmitDuringReset');Fails=$true}
  @{n='recovery-protocol-ReadAfterRelease';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-ReadAfterRelease');Fails=$true}
  @{n='recovery-protocol-KeepCoveredBoundary';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-KeepCoveredBoundary');Fails=$true}
  @{n='recovery-protocol-IgnoreReportInFlight';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-IgnoreReportInFlight');Fails=$true}
  @{n='recovery-protocol-IgnoreLostReport';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-IgnoreLostReport');Fails=$true}
  @{n='recovery-protocol-UseNotifiedAsCompleted';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-UseNotifiedAsCompleted');Fails=$true}
  @{n='recovery-protocol-ShareNodeWatermark';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-ShareNodeWatermark');Fails=$true}
  @{n='recovery-protocol-ResetWritesCompleted';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-ResetWritesCompleted');Fails=$true}
  @{n='recovery-protocol-IgnoreEpoch';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-IgnoreEpoch');Fails=$true}
  @{n='recovery-protocol-TimeoutAfterUnlock';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-TimeoutAfterUnlock');Fails=$true}
  # BD-120: the timeout latch marks the close the reset owns, and only the recovered path reopens it.
  @{n='recovery-protocol-LatchFaultsStickily';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-LatchFaultsStickily');Fails=$true}
  @{n='recovery-protocol-DropWitnessReopen';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-DropWitnessReopen');Fails=$true}
  @{n='recovery-protocol-ReopenWitnessOnRefusal';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-ReopenWitnessOnRefusal');Fails=$true}
  @{n='recovery-protocol-RefusalClosesEveryNode';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-RefusalClosesEveryNode');Fails=$true}
  @{n='submit-watchdog-flat-500';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-FlatFiveHundred');Fails=$true}
  @{n='submit-watchdog-no-tdr-floor';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-NoTdrFloor');Fails=$true}
  @{n='submit-watchdog-clamp-tdr';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-ClampTdrToBudgetMax');Fails=$true}
  @{n='submit-watchdog-ignore-progress';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-IgnoreProgress');Fails=$true}
  @{n='submit-watchdog-stamp-at-ring-write';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-StampAtRingWrite');Fails=$true}
  @{n='submit-watchdog-report-with-pending';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-ReportWithPendingCompletion');Fails=$true}
  @{n='submit-watchdog-no-fence-range';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-NoFenceRange');Fails=$true}
  @{n='submit-watchdog-log-the-constant';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-LogTheConstant');Fails=$true}
  @{n='submit-watchdog-rearm-outside-lock';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-RearmOutsideLock');Fails=$true}
  @{n='vmid-pool-ignore-retirement';s='driver\kmd\test\run_vmid_pool.ps1';a=@('-Root',$Workspace,'-IgnoreRetirement');Fails=$true}
  @{n='gfx-pipeline-idle-only';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-IdleOnlyPresent');Fails=$true}
  @{n='gfx-pipeline-no-capacity';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-WithoutCapacity');Fails=$true}
  @{n='gfx-pipeline-equal-fence';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-EqualityFence');Fails=$true}
  # -Root as well: this runner puts the compiler's TEMP under it, and a worktree's parent has no scratch\tmp.
  @{n='firmware-metadata-no-runtime';s='driver\kmd\test\run_firmware_metadata.ps1';a=@('-Root',$Workspace,'-Kits',$kits,'-Firmware',$firmware,'-OmitRuntimeFirmware');Fails=$true}
  @{n='paging-mc';s='driver\kmd\test\run_paging_mc.ps1';a=@('-Kits',$kits)}
  @{n='paging-private';s='driver\kmd\test\run_paging_private.ps1';a=@('-Kits',$kits)}
  @{n='paging-private-slot-reuse';s='driver\kmd\test\run_paging_private.ps1';a=@('-Kits',$kits,'-ReuseFirstQueueSlot');Fails=$true}
  @{n='paging-pt-shadow';s='driver\kmd\test\run_paging_pt_shadow.ps1';a=@('-Kits',$kits)}
  @{n='paging-stream';s='driver\kmd\test\run_paging_stream.ps1';a=@('-Kits',$kits)}
  @{n='paging-window';s='driver\kmd\test\run_paging_window.ps1';a=@('-Kits',$kits)}
  @{n='shim-replay';s='driver\shim\test\run.ps1';a=@('-Kits',$kits)}
  @{n='shim-gfx';s='driver\shim\test\run_gfx.ps1';a=@('-Kits',$kits)}
  # The frozen gfx job frame and the VM flush on the ring (docs/design/gfx-submit-root-serialization.md).
  @{n='gfx-vm-flush';s='driver\shim\test\run_gfx_vm_flush.ps1';a=@('-Root',$Workspace,'-Kits',$kits)}
  @{n='gfx-vm-flush-wrong-ack';s='driver\shim\test\run_gfx_vm_flush.ps1';a=@('-Root',$Workspace,'-Kits',$kits,'-WrongAckMask');Fails=$true}
  @{n='shim-ih';s='driver\shim\test\run_ih.ps1';a=@('-Kits',$kits)}
  @{n='shim-psp';s='driver\shim\test\run_psp.ps1';a=@('-Kits',$kits,'-Firmware',$firmware)}
  @{n='shim-pte';s='driver\shim\test\run_pte.ps1';a=@('-Kits',$kits)}
  @{n='shim-paging';s='driver\shim\test\run_paging.ps1';a=@('-Kits',$kits)}
  @{n='sdma-copy';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$kits)}
  @{n='sdma-copy-vmid-zero';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$kits,'-ForceIbVmidZero');Fails=$true}
  @{n='sdma-copy-no-vm-flush';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$kits,'-OmitVmFlush');Fails=$true}
  @{n='sdma-faults';s='driver\shim\test\run_sdma_faults.ps1';a=@('-Kits',$kits)}
  @{n='smu-mailbox';s='driver\shim\test\run_smu.ps1';a=@('-Root',$Workspace)}
  @{n='board-sequence';s='driver\shim\test\run_board_sequence.ps1';a=@('-Root',$Workspace)}
  @{n='board-sequence-changed-point';s='driver\shim\test\run_board_sequence.ps1';a=@('-Root',$Workspace,'-Mutation','gpu-point');Fails=$true}
  @{n='smu-native-board-admission';s='driver\shim\test\run_smu_native.ps1';a=@('-Root',$Workspace,'-Mutation','board-admission');Fails=$true}
  # The clock escape's two gates (BD-062): the token in front of the write, and the published reading the open
  # READ rests on. Each mutation removes one of them, and the suite must fail by a check; a mutation that nobody
  # runs is not a control, so both are listed here and not only in the runner.
  @{n='smu-native-clock-write-privilege';s='driver\shim\test\run_smu_native.ps1';a=@('-Root',$Workspace,'-Mutation','clock-write-privilege');Fails=$true}
  @{n='smu-native-clock-read-snapshot';s='driver\shim\test\run_smu_native.ps1';a=@('-Root',$Workspace,'-Mutation','clock-read-snapshot');Fails=$true}
  @{n='hwmon';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits)}
  @{n='hwmon-board-port';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits,'-Mutation','board-port');Fails=$true}
  # The case fan's load feed-forward (rule 10 of driver\shim\include\bc250_fan.h): the first negative controls of
  # the fan suite, because this is the rule that writes 255 to the chip by itself. Each -Mutation changes one line
  # of a copy of the file under test and the suite must then fail by a check.
  @{n='hwmon-no-boost-raise';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits,'-Mutation','no-boost-raise');Fails=$true}
  @{n='hwmon-boost-held-back';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits,'-Mutation','boost-held-back');Fails=$true}
  @{n='hwmon-step-pays-all';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits,'-Mutation','step-pays-all');Fails=$true}
  @{n='hwmon-telemetry-always';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits,'-Mutation','telemetry-always');Fails=$true}
  @{n='hwmon-boost-claims-fixed';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits,'-Mutation','boost-claims-fixed');Fails=$true}
  @{n='hwmon-rise-outlives-doubt';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$kits,'-Mutation','rise-outlives-doubt');Fails=$true}
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
 foreach($suite in $suites) {
  $gateJobs+=@{name=$suite.n;runner=$suite.s;arguments=@($suite.a);fails=[bool]$suite.Fails}
 }
 $variables=@{}
 foreach($name in @('repo','Workspace','Out','Kits','Profile','icd','radvBuild','umdSource','umdBuild','routerHosted','firmware')) {
  $variables[$name]=(Get-Variable -Name $name).Value
 }
 $vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
 if(!$vs){throw 'Visual Studio not found'}
 # Host scripts select the newest MSVC directory; the cache hashes that exact tree too.
 $compiler=(Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
 $planPath=Join-Path $Out 'plan.json'
 @{variables=$variables;context=$context;compiler=$compiler;jobs=$gateJobs;skips=$results} |
   ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $planPath -Encoding utf8
 if($PlanOnly){return}
 $argsGate=@('-B',"$repo/tools/quality/gate_cache.py",'--plan',$planPath,'--jobs',$Jobs)
 if($NoCache){$argsGate+='--no-cache'}
 & python @argsGate
 exit $LASTEXITCODE
} catch {
 @{status='FAIL';profile=$Profile;kits=$Kits;context=$context;seconds=$clock.Elapsed.TotalSeconds;checks=$results;error=$_.Exception.Message} | ConvertTo-Json -Depth 5 | Set-Content "$Out\result.json"
 Write-Error $_
 exit 1
}
