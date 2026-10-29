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
# The user-mode driver's own Mesa tree and its build database, neither of which is in this repository.
# BC250_UMD_SOURCE and BC250_UMD_BUILD name them: the umd-gate tree of the release (Mesa at the commit of its zink-umd
# row, configured with that row's arguments; build-release-set.ps1 -Package sets both from -UmdSource and -UmdBuild).
# There is no default. Each default so far named the tree of one train (scratch\m13-native-zink-src, then
# scratch\train\b34-build), and a cleanup removed part of it: the three gates then failed on a missing file that
# named no input. Without the variables, or with a tree that lacks a file that the gates read, those gates fail and
# give that reason (CheckUmd).
$umdSource=$env:BC250_UMD_SOURCE
$umdBuild=$env:BC250_UMD_BUILD
# Three host gates compile a fixture with cl.exe out of the environment. In a shell that is not a developer shell
# there is no cl.exe, and they died on a FileNotFoundError that named no input. This is the input they need.
$clExe=if($c=Get-Command cl.exe -ErrorAction SilentlyContinue){$c.Source}else{'cl.exe (the MSVC compiler is not in this environment)'}
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
# A gate on the user-mode driver tree. It is a release gate, so a missing input is a failure, not a skip: the
# reason names the input and the two variables.
function CheckUmd([string]$name,[string[]]$needs,[scriptblock]$action) {
    $reason=if(!$umdSource -or !$umdBuild){
        'BC250_UMD_SOURCE and BC250_UMD_BUILD are not set: set them to the umd-gate tree of the release (Mesa at the zink-umd row commit and its configured build)'
    } else {
        $absent=@($needs | Where-Object { !(Test-Path -LiteralPath $_) })
        if($absent){ "missing input: $($absent -join ', ') (BC250_UMD_SOURCE=$umdSource, BC250_UMD_BUILD=$umdBuild)" }
    }
    if(!$reason){ Check $name $action; return }
    $script:results+=@{name=$name;status='FAIL';reason=$reason}
    Write-Host "$name FAIL ($reason)"
}
try {
 # A named early gate makes a regression in link input order fail before builds.
 # quality-controls also discovers the positive and negative package unit controls.
 Check 'x86-callconv' { & pwsh -NoProfile -File "$repo/tools/build/test-x86-callconv.ps1" -OutputDir "$Out/x86-callconv" }
 # The two engine DLLs of one source line (BD-167): each flavour's own pin, and no run against the other's DLL.
 Check 'engine-abi-flavour' { & python -m unittest discover -s "$repo\tools\build" -p 'test_engine_abi.py' }
 # The vkd3d build id: a failed git describe (the fallback 0x12345678) or a dirty tree stops the engine build,
 # and the release refuses an engine record without the id of its commit.
 Check 'vkd3d-build-id' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\vkd3d-build-id").FullName; & python -B -m unittest discover -s "$repo\tools\build" -p 'test_vkd3d_build_id.py' }
 Check 'kmd-order' { & python "$repo\tools\quality\test_kmd_reproducible.py" --out "$Out\kmd-order" }
 # BD-218: source regression guard only; real HVCI acceptance stays separate.
 Check 'kmd-nx' { & python -B "$repo\tools\quality\kmd_nx.py" --root $repo --out "$Out\kmd-nx"; if($LASTEXITCODE -eq 0){ & python -B -m unittest discover -s "$repo\tools\quality" -p 'test_kmd_nx.py' } }
 Check 'kmd-pe-tests' { & python -B -m unittest discover -s "$repo\tools\quality" -p 'test_kmd_pe.py' }
 # One namespace spans all KMD headers, including optional diagnostic commands.
 Check 'escape-commands' { & python "$repo\tools\quality\escape_commands.py" --root $repo --out "$Out\escape-commands"; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\quality" -p 'test_escape_commands.py' } }
 # Its deadline is 1200 s in gate-inputs.json, not the 180 s of the manifest: 57 host tests of Python that build and
 # scan synthetic PE sets and packages. Measured on 2026-10-28: 146 s and 189 s run alone in one lane, 180.9 s and a FAIL at the
 # old deadline in a KMD row of a package build, and 251.2 s, 252.2 s alone and 222.8 s under an added CPU and disk
 # load on the development PC, which other lanes also used. At 180 s the load of the machine decided the result. A
 # gate with no room over its own cost is a flaky gate. It first got 900 s; then two KMD package rows of the same
 # evening ran it in 348.7 s and 362.8 s, so it holds 1200 s, 3.3x the worst recorded run, as the ledger and
 # release-provenance gates do.
 Check 'flavour-controls' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\flavour-controls").FullName; & python -B -m unittest discover -s "$repo\tools\quality" -p 'test_flavour_gate.py' }
 # Its deadline is 600 s in gate-inputs.json: 7 s to 37 s in the package rows of 2026-10-28, and 154.3 s in one
 # quick suite of that evening under the load of other lanes, 86 per cent of the old 180 s. 600 s is 3.9x that run.
 Check 'facts' { & python "$repo\tools\facts\gen_facts.py" --root $repo --check; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\facts" } }
 # Shares, denominators and modelled labels in the facts rows: a percentage must be the share of the
 # counts next to it, and a rate normalised to another clock must say which numbers the model made.
 Check 'claim-shares' { & python "$repo\tools\quality\claim_shares.py" --root $repo --out "$Out\claim-shares" }
 # The documentation style ratchet of docs/style.md: a document in tools\quality\doclint_baseline.txt
 # must not get worse, and a document outside it must be clean. It never asks for a rewrite.
 Check 'doclint' { & python "$repo\tools\quality\doclint.py" --root $repo --out "$Out\doclint" }
 # Status maps and release tool controls run in Full (required by build-release.ps1), not KmdPackage.
 Check 'status-map' { & python "$repo\tools\quality\kmd_status_profile.py" --repo $repo --profile $Profile --kits $Kits --out "$Out\status-map"; if($LASTEXITCODE -eq 0){ & python -m unittest discover -s "$repo\tools\docs" -p 'test_*.py' } }
 # Its deadline is 1200 s in gate-inputs.json. It had 300 s, lost five suites to that, and was given 600 s; 600 s
 # was not enough either, and this is the measurement that says so. 62 Python unit tests: 109 s run alone on an
 # idle machine, 144.8 s to 339.5 s over the 30 full suites recorded in this lane (five of the early ones died at
 # 300 s), and the gate's own cost has more than doubled across those runs as the work ledger grew. Under the 24
 # workers of the full profile it competes for the same cores as every other Python gate, and when another build
 # shares the machine that competition costs a further 1.8x: measured the same evening, kmd-deploy went from 102 s
 # to 184 s and router-front from 71 s to 129 s. 339.5 s times that factor is 625 s, which is over 600, so the
 # old deadline would have failed suites at random on any branch. 1200 s is 11x the cost alone and 3.5x the worst
 # suite run, which covers both the contention and the growth.
 Check 'ledger' { & python -m unittest discover -s "$repo\tools\win\ledger" }
 # Windows PowerShell 5.1 for the app-route test, because that is the shell its ops scripts run in on the lab.
 Check 'app-route-lib' { & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\win\app-route\ops\tests\test-approute-lib.ps1" -Out "$Out\app-route-lib" }
 # Settings apply/restore runs against a fake system. Never change this PC's services, tasks or registry.
 Check 'system-tuning' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\win\system-tuning\test-system-tuning.ps1" -Out "$Out\system-tuning" }
 Check 'system-tuning-installer' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\release\test-system-tuning-installer.ps1" -Out "$Out\system-tuning-installer" }
 # BD-211: which leaked MEMxxxx.TMP files the installer removes, in a folder under Out only (no real temp folder).
 Check 'mesa-temp-cleanup-installer' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\release\test-mesa-temp-cleanup-installer.ps1" -Out "$Out\mesa-temp-cleanup-installer" }
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
 # Its deadline is 900 s in gate-inputs.json. It had 300 s. The tester.33 release suites ran it in 196 s to 248 s one
 # flavour at a time, so 300 s left 20 % of margin. build-release-set.ps1 -Parallel runs the release and the lab
 # suites at the same time, and then all four runs of this gate stopped at 300 s. 900 s is 3.6x the worst serial run.
 Check 'quality-controls' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\quality-controls").FullName; & python -m unittest discover -s "$repo\tools\quality" }
 Check 'gate-cache-controls' { & python -B -m unittest discover -s "$repo\tools\quality" -p 'test_gate_cache.py' }
 Check 'wininstall-locale' { & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\wininstall\test-locale.ps1" -Out "$Out\wininstall-locale" }
 Check 'kd-dump-triage' { & pwsh -NoProfile -File "$repo\tools\win\kd\analyze-kernel-dump.ps1" -SelfTest }
 Check 'smartplug' { & python "$repo\tools\win\smartplug\selftest.py" }
 Check 'hostwatch' { & pwsh -NoProfile -File "$repo\tools\win\hostwatch\selftest.ps1" -Out "$Out\hostwatch" }
 # The emergency listener runs under Windows PowerShell 5.1 on the lab, so its parser is the one that must accept it.
 Check 'lab-emerg' { & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\win\lab-emerg\parse-check.ps1" }
 # Its deadline is 600 s in gate-inputs.json, not the 180 s of the manifest, for the reason the ledger and
 # release-provenance gates hold 600 s: 43 host tests of Python, which compete for the same cores as every
 # other Python gate under the 24 workers of the full profile. Measured: 50.7 s alone, 57.6 s to 102.0 s over
 # eight full suites, and 184 s - over the old deadline, with an empty log and no reason - in the one suite
 # that shared the machine with another eight-way build. The work did not grow; the room over its own cost
 # was never there. 600 s is 11.8 times its cost alone and 5.9 times its worst suite run, so the load of the
 # machine no longer decides it. (The commit that set it, ce623538, says "six times" and "twice" in its
 # message: those two multipliers are wrong, the measurements beside them are right, and 600 s is more room
 # than the message claimed rather than less.)
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
# Its deadline is 900 s in gate-inputs.json: 63 s to 91 s with a quiet machine, 135.6 s to 174.3 s in the KMD package
# rows of 2026-10-28 that shared the machine with other lanes, 97 per cent of the old 180 s. 900 s is 5.2x the worst.
 Check 'kmd-compile' { & pwsh -NoProfile -File "$repo\driver\kmd\build.ps1" -Kits "$Kits" -Out "$Out\kmd" -CompileOnly }
 # BD-166: the kernel-mode driver carries a compiled version resource, so a game that asks Windows which AMD driver
# it runs on does not read 0.0.0.0 (Counter-Strike 2 did). The numbers come from version-stamp.h, which the KMD
# build writes from the quad the release builder maps; this compiles the resource with the kit's own rc.exe, reads
# the numbers back out of the compiled bytes and holds the refusal of a resource built without them. Two seconds.
Check 'kmd-version-rc' { & python "$repo\tools\build\test_kmd_version_rc.py" --kits "$Kits" --out "$Out\kmd-version-rc" }
Check 'kmd-contract' { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$Out\kmd\compile_commands.json" --match '/driver/(kmd|shim)/' --out "$Out\kmd-contract" }
 Check 'guardlog-width' { & python "$repo\tools\quality\guardlog_width.py" --kmd "$repo\driver\kmd" --baseline "$repo\tools\quality\guardlog_width_baseline.txt" --out "$Out\guardlog-width" }
 # The failure statuses a KMD DDI can return, against its return table: SubmitCommandVirtual may fail with
 # STATUS_INVALID_PARAMETER only, any other failure is a bugcheck. --self-test also holds two mutants to a failure.
 Check 'ddi-returns' { & python "$repo\tools\quality\ddi_returns.py" --kmd "$repo\driver\kmd" --self-test }
 # The provenance gate of the release payload (tools/release/provenance.py, run by build-release.ps1): its self-test on
 # throwaway repositories. The gate itself needs the payload files of a workspace, so the release build runs it.
 # Its deadline is 600 s in gate-inputs.json, not the 180 s of the manifest: the work takes 113 s to 171 s on the
 # development PC (rc1, rc2, rc3 and two runs of rc4 of train b34), and at 180 s the load of the machine decided the
 # result. rc4 lost a whole package build to that margin. A gate with 10 per cent of room over its own cost is a
 # flaky gate, not a strict one. 300 s was not enough either: over the 15 full suites of rc1 to rc5 this gate ran
 # 149 s to 266 s and still lost two of them at 300 s, so it got 600 s. It now holds 1200 s, for the same reason
 # and from the same measurement as the ledger gate beside it: its worst recorded suite run is 329.4 s, and the
 # 1.8x a second build on the machine costs would put that at 600 s exactly. Both are Python unit tests among the
 # 24 workers of the full profile, so they are hit by the same load at the same time.
 Check 'release-provenance' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\release-provenance").FullName; & python -m unittest discover -s "$repo\tools\release" -p 'test_provenance.py' }
 # The release set build (tools/release/build-release-set.ps1) on stub recipes in temporary repositories: -MaxParallel and the
 # disk budget hold, -Plan builds nothing, an unpushed commit, an AI trailer and an implicit compiler are refused, and a
 # failed build fails the release set and keeps its log. Its end-to-end runs start pwsh processes, hence 600 s.
 Check 'release-build-set' { $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\release-build-set").FullName; & python -m unittest discover -s "$repo\tools\release" -p 'test_build_release_set.py' }
 Check 'release-driver-package' { $env:TEMP=(New-Item -ItemType Directory -Force "$Out\release-driver-package").FullName; $env:TMP=$env:TEMP; & python -m unittest discover -s "$repo\tools\release" -p 'test_driver_package.py' }
 Check 'release-source-versions' { $env:TEMP=(New-Item -ItemType Directory -Force "$Out\release-source-versions").FullName; $env:TMP=$env:TEMP; & python -m unittest discover -s "$repo\tools\release" -p 'test_source_versions.py' }
 # docs/testing/THIRD-PARTY.md names the commit of each payload file and the submodules of its build: the same commits
 # as release-sources.json and the gitlinks of those commits in the workspace checkouts, and no branch.
 Check 'third-party-doc' { & python -B "$repo\tools\release\third_party_doc.py" --repo $repo --workspace $Workspace; if($LASTEXITCODE -eq 0){ $env:BC250_TEST_OUT=(New-Item -ItemType Directory -Force "$Out\third-party-doc").FullName; & python -B -m unittest discover -s "$repo\tools\release" -p 'test_third_party_doc.py' } }
 Check 'sdk-compiler-arguments' { & pwsh -NoProfile -File "$repo\tools\build\test-sdk-compiler.ps1" }
 Check 'release-flavour' { & python -m unittest discover -s "$repo\tools\release" -p 'test_release_flavour.py' }
 # BD-167: the source lookup of the flavour byte check (tools/release/flavour_bytes.py) on a throwaway repository. A
 # commit or a checkout that Git cannot read is an error, never the absence of a lab string.
 Check 'release-flavour-bytes' { & python -m unittest discover -s "$repo\tools\release" -p 'test_flavour_bytes.py' }
 # BD-166: the D3D11 configuration record of a staged package (tools/release/check_package_v3.py). b15 shipped a new
 # ICD with the old record and every D3D11 device failed with ERROR_CRC, so the record is checked in the package that
 # ships. The test stages a package of its own images: no device and no machine state.
 Check 'release-package-v3' { $env:TEMP=(New-Item -ItemType Directory -Force "$Out\release-package-v3").FullName; $env:TMP=$env:TEMP; & python -m unittest discover -s "$repo\tools\release" -p 'test_check_package_v3.py' }
 # The symbols of the shipped files (tools/release/symbols.py): a crash report, a lab dump and a debugger session
 # need the symbols of the exact bytes that ran, so the release archives them beside its build and the manifest
 # says which file belongs to which byte. The controls run on crafted images: no compiler and no machine state.
 Check 'release-symbols' { $env:TEMP=(New-Item -ItemType Directory -Force "$Out\release-symbols").FullName; $env:TMP=$env:TEMP; & python -m unittest discover -s "$repo\tools\release" -p 'test_symbols.py' }
 # BD-166: the version resource that the release build gives every PE file of the package (tools/build/versionstamp.py),
 # on images that the test builds itself: the stamp, its read-back through the Windows image loader, the M14C pins.
 Check 'version-stamp' { & python -m unittest discover -s "$repo\tools\build" -p 'test_versionstamp.py' }
 # BD-166: which records a release must carry, and the refusal of a shipped PE that nothing in versioninfo.json
 # stamps, both through the public release gate (tools/build/versionstamp.py check_release).
 Check 'version-stamp-records' { & python -m unittest discover -s "$repo\tools\build" -p 'test_versionstamp_records.py' }
 # The driver package's own AddReg against the release defaults table (BD-091): every setting the INF writes is the
 # released value and carries NOCLOBBER, so no install outside our installer closes a gate while another stays open.
 Check 'inf-gates' { & python "$repo\tools\quality\inf_gates.py" --inf "$repo\driver\kmd\bc250kmd.inf" --defaults "$repo\tools\release\installer\registry-defaults.json" --out "$Out\inf-gates" }
 # BD-190: every setting the KMD reads is a catalog entry with the same code default. A name outside the catalog
 # reads as 0 at run time, which turned the boot logo and the lab diagnostics off without a log line.
 Check 'driver-state-settings' { & python "$repo\tools\state\check_settings.py" --root "$repo"; if($LASTEXITCODE -eq 0){ & python -I -m unittest discover -s "$repo\tools\state" -p 'test_check_settings.py' } }
 Check 'driver-state-x64' { & pwsh -NoProfile -File "$repo\tools\state\test.ps1" -Kits "$Kits" -Out "$Out\driver-state-x64" -Arch x64 }
 Check 'driver-state-x86' { & pwsh -NoProfile -File "$repo\tools\state\test.ps1" -Kits "$Kits" -Out "$Out\driver-state-x86" -Arch x86 }
 Check 'stored-fan-choice' { & pwsh -NoProfile -File "$repo\tools\state\test-fan-choice.ps1" -Kits "$Kits" -Out "$Out\stored-fan-choice" }
 Check 'driver-state-kernel' { & pwsh -NoProfile -File "$repo\tools\state\test-kernel.ps1" -Kits "$Kits" -Out "$Out\driver-state-kernel" }
 Check 'driver-state-package' { & powershell -NoProfile -NonInteractive -File "$repo\tools\release\test-state-package.ps1" -Out "$Out\driver-state-package" }
 Check 'driver-state-installer' { & pwsh -NoProfile -File "$repo\driver\state\installer-build.ps1" -Kits "$Kits" -Out "$Out\driver-state-installer" -Test }
 Check 'driver-state-lifecycle' { & powershell -NoProfile -NonInteractive -File "$repo\tools\release\test-state-lifecycle.ps1" }
 Check 'driver-state-rollback' { & powershell -NoProfile -NonInteractive -File "$repo\tools\release\test-legacy-rollback.ps1" }
 Check 'app-profiles-installer' { & powershell -NoProfile -NonInteractive -File "$repo\tools\release\test-app-profiles.ps1" }
 Check 'driver-state-consumer-syntax' { & powershell -NoProfile -NonInteractive -File "$repo\tools\state\test-consumer-syntax.ps1" }
 # Native consumers link the same state ABI. These gates never open a device or a window.
 Check 'driver-state-cli-x64' { & pwsh -NoProfile -File "$repo\tools\win\bc250kmd_cli\build.ps1" -Kits "$Kits" -Out "$Out\driver-state-cli-x64" -Arch x64 }
 Check 'driver-state-cli-x86' { & pwsh -NoProfile -File "$repo\tools\win\bc250kmd_cli\build.ps1" -Kits "$Kits" -Out "$Out\driver-state-cli-x86" -Arch x86 }
 # The lab flavour of the CLI (BD-167: the raw diagnostic verbs) compiles too; test-flavour.ps1 checks both builds.
 Check 'driver-state-cli-lab-x64' { & pwsh -NoProfile -File "$repo\tools\win\bc250kmd_cli\build.ps1" -Kits "$Kits" -Out "$Out\driver-state-cli-lab-x64" -Arch x64 -Lab }
 Check 'driver-state-cumode-x64' { & pwsh -NoProfile -File "$repo\tools\win\cumode\build.ps1" -Kits "$Kits" -Out "$Out\driver-state-cumode-x64" -Arch x64 }
 Check 'driver-state-cumode-x86' { & pwsh -NoProfile -File "$repo\tools\win\cumode\build.ps1" -Kits "$Kits" -Out "$Out\driver-state-cumode-x86" -Arch x86 }
 Check 'driver-state-client-x64' { & pwsh -NoProfile -File "$repo\tools\state\test-client.ps1" -Kits "$Kits" -Out "$Out\driver-state-client-x64" -Arch x64 }
 Check 'driver-state-client-x86' { & pwsh -NoProfile -File "$repo\tools\state\test-client.ps1" -Kits "$Kits" -Out "$Out\driver-state-client-x86" -Arch x86 }
 Check 'driver-state-gui-batch' { & pwsh -NoProfile -File "$repo\tools\state\test-gui-batch.ps1" -Kits "$Kits" -Out "$Out\driver-state-gui-batch" }
 Check 'driver-state-overlay' {
  & pwsh -NoProfile -File "$repo\tools\win\bc250mon\test-state.ps1" -Out "$Out\driver-state-overlay"
  if($LASTEXITCODE -eq 0){ & pwsh -NoProfile -File "$repo\tools\win\bc250mon\test-lab-state.ps1" -Out "$Out\driver-state-overlay-lab" }
 }
 Check 'driver-state-infverif' {
  $infverif=Join-Path $Kits 'microsoft.windows.wdk.x64\c\tools\10.0.26100.0\x64\infverif.exe'
  if(-not (Test-Path -LiteralPath $infverif)){throw "InfVerif is required: $infverif"}
  & $infverif /h /v "$repo\driver\kmd\bc250kmd.inf"
 }
 # Upgrade defaults and explicit clock choices must survive the same registry writes as an installation.
 Check 'registry-defaults' { & powershell -NoProfile -File "$repo\tools\release\test-registry-defaults.ps1" }
# The work ledger gate must judge the manifest the builder is building from, not the published one. This parses
# build-release.ps1 and demands that the ledger call and the manifest read name one variable, so a path that is
# passed but wrong fails here instead of turning a train's own integrations into release debt.
Check 'ledger-call' { & pwsh -NoProfile -File "$repo\tools\release\test-ledger-call.ps1" -Out "$Out\ledger-call" }
 # BD-077: the installer half of the CU mode setting. The -CuMode switch must reach the registry plan, and
 # docs\testing\INSTALL.md must state the number of compute units of an install that does not use the switch.
 Check 'cu-mode-installer' { & powershell -NoProfile -File "$repo\tools\release\test-cu-mode-installer.ps1" -Repo $repo }
 # BD-183: the payload directories an install copies. rc5 of tester.31 would not install on 26200: the DriverStore
 # layout selects one directory, PowerShell unrolled that one-element list to a string, and the two += that follow
 # concatenated three names into '...\payload\toolssystem-tuningcontrol'. One decision in one function now, and
 # this holds it to a list even when it holds one name.
 Check 'install-payload-dirs' { & powershell -NoProfile -File "$repo\tools\release\test-install-dirs.ps1" }
 # BD-122: the consumers of the KMD's start record (start-confirm and the installer's verify), with fixtures in a
 # scratch key of HKCU that it removes again. Windows PowerShell 5.1, the shell a tester's installer runs in.
 Check 'start-record-consumers' { & powershell -NoProfile -File "$repo\tools\release\test-start-record.ps1" }
 # The installer's own diagnostics of a start that did not pass (the decision, the readers and
 # the bounds), with the field case as a fixture. Windows PowerShell 5.1, the shell a tester runs.
 Check 'installer-diagnostics' { & powershell -NoProfile -File "$repo\tools\release\test-diagnostics.ps1" }
 # BD-060 and BD-080: the INF restart directive, the resumed install's inputs, the DWM baseline, and the plan of the
 # two logon tasks (who confirms a driver start, and at whose logon). It writes files under Out and registers no
 # task. Until now it ran only inside test-dryrun.ps1, which needs a built package.
 Check 'session-checks' { & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$repo\tools\release\test-session-checks.ps1" -WorkRoot "$Out\session-checks" }
 # Fact M898: a WinForms Label with a Dock and AutoSize off goes through 100x23 at each change of its text, which is
 # how the start window of rc4 flashed. The control application holds the rule at run time (--smoke-labels in its
 # build); this reads the installer's and the tools' PowerShell and the other C# windows. Its controls run in
 # quality-controls (test_winforms_labels.py).
 Check 'winforms-labels' { & python "$repo\tools\quality\winforms_labels.py" --root $repo }
 # The application the tester clicks was in no gate of this suite, and two defects of the b34 merges reached the
 # tree because of it: the source list of its two compiles lost its assignment, so the release flavour's lab twin
 # compiled no file at all (CS5001), and Crash.cs read a lab-only setting in every flavour, so the release image
 # carried the name of a diagnostic variable. Both were found by hand, days apart. These three gates close that:
 # the build itself, and the two static rules whose cost is seconds and which name the line.
 #
 # control-app is the build with -NoSmoke: the unit tests, the native DLL and CLI compiles, both flavours of the
 # application, the lab twin, and the image's flavour check. -NoSmoke leaves out only what starts the application
 # as a process (the smoke, report, recovery, perf, notice, loading, scroll and render runs), which needs a desktop
 # and minutes of it; the release build still runs all of them. It also passes -StartConfirmCore, which nothing
 # else does, so the rule the installer confirms a driver start by is read here too. Its deadline is 600 s, not
 # the 180 s of the manifest: the build takes 72 s on the idle development PC and this gate competes with 23
 # others for the same cores, and a gate with no room over its own cost is a flaky gate (the same reason the
 # ledger and release-provenance gates hold 600 s).
 Check 'control-app' { & pwsh -NoProfile -File "$repo\tools\win\amdgpu_wddm_control\build.ps1" -Kits "$Kits" -Out "$Out\control-app" -NoSmoke -StartConfirmCore "$repo\tools\release\installer\start-confirm-core.ps1" }
 # Every variable a build recipe reads is written somewhere in that recipe (tools\quality\check-unassigned.ps1,
 # PowerShell's own parser). This is the rule the lost assignment broke, and it reads all 105 recipes in 3 s.
 Check 'script-variables' { & pwsh -NoProfile -File "$repo\tools\quality\check-unassigned.ps1" -Recipes -Repo $repo }
 # One variable has one spelling in each scope that writes it (tools\quality\check-variable-case.ps1, PowerShell's own
 # parser). PowerShell variable names ignore case, so $report = @{...} wrote a table into the [string]$Report
 # parameter of build-release-set.ps1 and 4 of its 32 tests failed while a package was cut. It reads every tracked
 # .ps1 and .psm1 of the repository except the records, about 580 files in 12 s on the idle development PC and 42 s
 # under load. Its deadline is its own 300 s in gate-inputs.json, outside quality-controls, which runs only its
 # planted controls. It declares no inputs: it reads the file list of git and scripts in every directory, which no
 # input group covers, so it runs in every suite.
 Check 'variable-case' { & pwsh -NoProfile -File "$repo\tools\quality\check-variable-case.ps1" -Repo $repo }
 # Every read of one of the four lab-only settings in the control application's sources is inside its flavour
 # guard (tools\quality\flavour_sources.py). The build proves the same thing on the built image; this names the
 # file and the line without a compiler. Both gates' planted controls run in quality-controls.
 Check 'flavour-sources' { & python "$repo\tools\quality\flavour_sources.py" --root $repo --out "$Out\flavour-sources" }
 # Every recipe config resolves in both flavours, and every read of a config's key finds the key or looks it up
 # first (tools\quality\check-recipe-configs.ps1). Under the Set-StrictMode 3.0 of common.ps1 an absent key is an
 # error: the release flavour of nine configs stopped on release_options before meson ran, and three configs would
 # have stopped on the ships map after ninja in either flavour. It runs each script's own option resolution, 150 of
 # them (build-mesa.ps1 with each compiler and variant), needs no source checkout and compiles nothing, in 3 s. Its
 # planted controls run in quality-controls.
 Check 'recipe-configs' { & pwsh -NoProfile -File "$repo\tools\quality\check-recipe-configs.ps1" -Repo $repo -Out "$Out\recipe-configs" }
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
 # The same case as radv-contract above: the user-mode driver's build database is not in the repository.
 CheckUmd 'umd-contract' @("$umdBuild\compile_commands.json") `
   { & python "$repo\tools\quality\prototype_gate.py" --compile-commands "$umdBuild\compile_commands.json" --match '/src/gallium/(frontends/d3d10umd|targets/d3d10umd|drivers/zink)/' --out "$Out\umd-contract" }
 Check 'vsync-vector' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vsync_vector.ps1" -Root $Workspace -Out "$Out\vsync-vector" }
Check 'paging-ddi-status' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_ddi_status.ps1" -Root $Workspace -Out "$Out\paging-ddi-status" }
Check 'paging-ddi-status-refusal-success' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_ddi_status.ps1" -Root $Workspace -Out "$Out\paging-ddi-status-refusal-success" -Mutation refusal-success }
Check 'paging-ddi-status-pending-success' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_ddi_status.ps1" -Root $Workspace -Out "$Out\paging-ddi-status-pending-success" -Mutation pending-success }
# BD-235: the rules of the bounded host test runner (host_native.ps1) with child fixtures: exit 0 or 1 to go with the
# summary, one summary, the FAIL count, the deadline.
Check 'host-native' { & pwsh -NoProfile -File "$repo\driver\kmd\test\test_host_native.ps1" -Root $Workspace -Out "$Out\host-native" }
# BD-235: the BuildPagingBuffer statuses through wddm.c's own DDI code, and three negative controls (the rules before Q3
# and Q3a).
Check 'paging-build' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_build.ps1" -Root $Workspace -Out "$Out\paging-build" }
Check 'paging-build-absorb-inner-failure' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_build.ps1" -Root $Workspace -Out "$Out\paging-build-absorb-inner-failure" -Mutation absorb-inner-failure }
Check 'paging-build-cpu-virtual-ignore' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_build.ps1" -Root $Workspace -Out "$Out\paging-build-cpu-virtual-ignore" -Mutation cpu-virtual-ignore }
Check 'paging-build-unmarked-failure' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_build.ps1" -Root $Workspace -Out "$Out\paging-build-unmarked-failure" -Mutation unmarked-failure }
# BD-235 Q3a: the journal's stamps pass a failed build's record, also when the OS reuses its buffer range.
Check 'paging-journal' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_journal.ps1" -Root $Workspace -Out "$Out\paging-journal" }
Check 'paging-journal-stamp-failed' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_journal.ps1" -Root $Workspace -Out "$Out\paging-journal-stamp-failed" -Mutation stamp-failed }
# BD-235: no raw ring writer runs beside OS paging. The SDMA0 writers, their paths and the orders they need (with five
# controls of its own), and the escapes admitted under FullWddm in both flavours, with a negative control.
Check 'sdma0-writers' { & python -B "$repo\driver\kmd\test\check_sdma0_writers.py" --source "$repo\driver\kmd" }
Check 'escape-admission' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_escape_admission.ps1" -Root $Workspace -Out "$Out\escape-admission" }
Check 'escape-admission-admit-fence' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_escape_admission.ps1" -Root $Workspace -Out "$Out\escape-admission-admit-fence" -Mutation admit-fence }
 Check 'ih-consume' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_ih_consume.ps1" -Root $Workspace -Out "$Out\ih-consume" }
 Check 'dpm' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_dpm.ps1" -Out "$Out\dpm" -Kits "$Kits" }
 Check 'cpu' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_cpu.ps1" -Out "$Out\cpu" -Kits "$Kits" }
 Check 'clock-policy' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_clock.ps1" -Root $Workspace -Out "$Out\clock-policy" }
 Check 'oc-tiers' { & pwsh -NoProfile -File "$repo\driver\shim\test\run_oc.ps1" -Root $Workspace -Out "$Out\oc-tiers" }
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
 # F-q44-mem-4: a rejected node-0 submit (STATUS_INVALID_PARAMETER) completes only after the packets ahead of it.
 Check 'paging-preemption-early-reject' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_paging_preemption.ps1" -Root $Workspace -Out "$Out\paging-preemption-early-reject" -Mutation '--early-reject' -ExpectFailure }
 Check 'surface-layout' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dcn_translate.ps1" -Root $Workspace -Out "$Out\surface-layout" -Kits "$Kits" }
 Check 'gdi-admission' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gdi_admission.ps1" -Root $Workspace -Out "$Out\gdi-admission" -Kits "$Kits" }
 # F-q44-mem-3: the actual OpenAllocation with the pool failed at the first, a middle and the last allocation, and
 # one control per property: the NO_MEMORY answer, the freeing of the earlier objects, the per-call counts.
 Check 'open-allocation' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_open_allocation.ps1" -Root $Workspace -Out "$Out\open-allocation" -Kits "$Kits" }
 Check 'open-allocation-success-null' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_open_allocation.ps1" -Root $Workspace -Out "$Out\open-allocation-success-null" -Kits "$Kits" -Mutation '--success-null' -ExpectFailure }
 Check 'open-allocation-keep-earlier' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_open_allocation.ps1" -Root $Workspace -Out "$Out\open-allocation-keep-earlier" -Kits "$Kits" -Mutation '--keep-earlier' -ExpectFailure }
 Check 'open-allocation-count-at-once' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_open_allocation.ps1" -Root $Workspace -Out "$Out\open-allocation-count-at-once" -Kits "$Kits" -Mutation '--count-at-once' -ExpectFailure }
 Check 'gdi-render' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gdi_render.ps1" -Root $Workspace -Out "$Out\gdi-render" -Kits "$Kits" }
 # The walk of the OS command buffer, with the page behind the buffer inaccessible: the review r1
 # over-read was invisible to a test that only compares return values (driver\kmd\gdi_walk.h).
 Check 'gdi-walk' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gdi_walk.ps1" -Root $Workspace -Out "$Out\gdi-walk" -Kits "$Kits" }
 # The GDI compute path: the code object gate (which rebuilds the kernels where the device compiler
 # is present and checks the recorded hashes where it is not) and the dispatch host test.
 Check 'gdi-kernels' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gdi_kernels.ps1" -Root $Workspace -Out "$Out\gdi-kernels" -Kits "$Kits" }
 Check 'display-modes' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_display_modes.ps1" -Root $Workspace -Out "$Out\display-modes" -Kits "$Kits" }
 Check 'vidpn-flip' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip" }
 # BD-170 r6: each rule of wddm.c's flip state machine broken in a copy of the extracted source; every one must fail.
 # Its deadline is 900 s in gate-inputs.json: 57 s to 168.4 s over the passing runs of 2026-10-28, and three FAILs at
 # the old 180 s in KMD package rows under load. 900 s is 5.3x the worst passing run.
 Check 'vidpn-flip-controls' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip-controls" -Mutation all }
 # The lab flavour's witnesses (BD-167 switch): read after the shipping decision, never change it; each control must fail.
 Check 'vidpn-flip-lab' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip-lab" -Lab }
 Check 'vidpn-flip-lab-controls' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_vidpn_flip.ps1" -Root $Workspace -Out "$Out\vidpn-flip-lab-controls" -Lab -Mutation all }
 Check 'scanout-admit' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_scanout_admit.ps1" -Root $Workspace -Out "$Out\scanout-admit" -Kits "$Kits" }
 Check 'dpaudio' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dpaudio.ps1" -Root $Workspace -Out "$Out\dpaudio" -Kits "$Kits" }
 Check 'modeset' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_modeset.ps1" -Root $Workspace -Out "$Out\modeset" -Kits "$Kits" }
 # BD-223: the scaler, MPC, surface and colour helpers, the DC types and the latch probe are gates of their own. Inside
 # modeset they made it run past its 180 s deadline.
 Check 'scaler-geometry' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_scaler_geometry.ps1" -Root $Workspace -Out "$Out\scaler-geometry" }
 Check 'dc-types' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_dc_types.ps1" -Root $Workspace -Out "$Out\dc-types" }
 Check 'latch-probe' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_latch_probe.ps1" -Root $Workspace -Out "$Out\latch-probe" -Mutations }
 Check 'atom-display' {
  $atomArgs=@('-Root',$Workspace,'-Out',"$Out\atom-display",'-Kits',$Kits)
  if($env:BC250_LINK_TEST_ROM){$atomArgs+=@('-PrivateRom',$env:BC250_LINK_TEST_ROM)}
  & pwsh -NoProfile -File "$repo\driver\kmd\test\run_atom.ps1" @atomArgs
 }
 Check 'link-command-import' { & python -B "$repo\tools\import\extract_link_commands.py" --check }
 Check 'link-register-import' { & python -B "$repo\tools\import\extract_link_regs.py" --check }
 Check 'blit-plan' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_blit_plan.ps1" -Root $Workspace -Out "$Out\blit-plan" -Kits "$Kits" }
 # The boot-screen mascot (docs/design/boot-logo.md): the run-length decoder, the placement and the blend, against
 # an independent pixel oracle, plus the shipped asset's own self-check and its size budget.
 Check 'boot-logo' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_boot_logo.ps1" -Root $Workspace -Out "$Out\boot-logo" -Kits "$Kits" }
 Check 'gpu-clock' { & pwsh -NoProfile -File "$repo\driver\kmd\test\run_gpu_clock.ps1" -Root $Workspace -Out "$Out\gpu-clock" -Kits "$Kits" }
 # The installer owns version mapping; KMD must not rewrite OS device properties.
 Check 'driver-version' { & powershell -NoProfile -File "$repo\tools\release\test-version-identity.ps1" }
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
     foreach($suite in @('policy','front-tables','front-tables13','front-rule','front-record')){
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
 # The open ADL subset (docs/design/adl-subset.md) for both architectures: the export, dependency and calling-
 # convention gates, the unit test, and the real AGS 6.3 library over the shipped, the injected and the two
 # negative-control builds. AGS is not in the repository (THIRD-PARTY.md); the dx12 case needs a D3D12 adapter.
# Its deadline is 900 s in gate-inputs.json: two builds and their tests, 85 s to 178.2 s over the passing runs of
# 2026-10-28 (146 s alone in one lane), and five FAILs at the old 180 s in KMD package rows under load. 900 s is 5x
# the worst passing run.
 CheckIf 'umd-adl' "$Workspace\ref\AGS_SDK-v6.3.0\amd_ags.h" {
     $env:BC250_ROOT=$Workspace
     foreach($arch in @('x64','x86')){
         $build=Join-Path $Out "umd-adl-$arch"
         & pwsh -NoProfile -File "$repo\tools\build\build-umd-adl.ps1" -Arch $arch -OutputDir $build
         if($LASTEXITCODE -ne 0){ exit 1 }
         & pwsh -NoProfile -File "$repo\tools\build\test-umd-adl-ags.ps1" -Arch $arch -BuildDir $build
         if($LASTEXITCODE -ne 0){ exit 1 }
     }
 }
 Check 'escape-abi' { & python -m unittest discover -s "$repo\tools\win\bc250kmd_cli" }
 # These three compile a C fixture of driver\kmd\test with cl.exe and run it. The compiler is the input.
 CheckIf 'allocation-identity' $clExe { & python "$repo\tools\quality\allocation_identity.py" --out "$Out\allocation-identity" }
 CheckIf 'object-index' $clExe { & python "$repo\tools\quality\object_index.py" --out "$Out\object-index" }
 CheckIf 'hang-witness' $clExe { & python "$repo\tools\quality\hang_witness.py" --out "$Out\hang-witness" }
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
 CheckUmd 'surface-control' @("$umdSource\src\gallium\auxiliary\util\u_inlines.h") { & python "$PSScriptRoot\check_surface.py" --mesa "$umdSource" --out "$Out\surface-control" }
 # kmd-analysis reads the database the kmd-compile gate of this same run writes, so it has no missing-input case:
 # a gate that ran before it in this profile either wrote the file or failed itself.
 Check 'kmd-analysis' { & python "$PSScriptRoot\msvc_analysis.py" --database "$Out\kmd\compile_commands.json" --match '/umd_blob.c$' --out "$Out\analysis-kmd" }
 # The WDK headers reach the user-mode driver's own build through INCLUDE, not through a -I of its compile command
 # (tools\build\build-mesa.ps1 puts them there). A gate that replays one of those commands needs them there too,
 # or it dies on a missing d3d10umddi.h and says nothing about the code it was to judge, which is what it did in
 # train b34 rc1 to rc4. The kit of this workspace is the one the build used.
 CheckUmd 'umd-analysis' @("$umdBuild\compile_commands.json", "$umdSource\src\gallium\frontends\d3d10umd\Device.cpp") `
   { $wdk=Join-Path $Kits 'microsoft.windows.wdk.x64\c\Include\10.0.26100.0'
     $env:INCLUDE=((@((Join-Path $wdk 'um'), (Join-Path $wdk 'shared')) + @($env:INCLUDE)) -join ';')
     & python "$PSScriptRoot\msvc_analysis.py" --database "$umdBuild\compile_commands.json" --match '/Device.cpp$' --out "$Out\analysis-umd" --clang-baseline "$PSScriptRoot\clang-umd-baseline.json" }
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
  @{n='connector-replug';s='driver\kmd\test\run_connector_replug.ps1';a=@('-Kits',$Kits)}
  @{n='lab-gate';s='driver\kmd\test\run_lab_gate.ps1';a=@('-Kits',$Kits)}
  @{n='connector-modes';s='driver\kmd\test\run_connector_modes.ps1';a=@('-Root',$Workspace)}
  @{n='connector-link';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits)}
  @{n='connector-link-stale-generation';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitGeneration','-ExpectFailure')}
  @{n='connector-link-open-cleanup';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitOpenCleanup','-ExpectFailure')}
  @{n='connector-link-cold-recovery';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitColdRecovery','-ExpectFailure')}
  @{n='connector-link-prepare-cleanup';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitPrepareCleanup','-ExpectFailure')}
  @{n='connector-link-first-failure';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitFirstFailure','-ExpectFailure')}
  @{n='connector-link-nested-hdmi';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-NestedHdmiStop','-ExpectFailure')}
  @{n='connector-link-diagnostic-replay';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitDiagnosticReplay','-ExpectFailure')}
  @{n='connector-link-write-requested';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-WriteRequestedValue','-ExpectFailure')}
  @{n='connector-link-recovery-target';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-WrongRecoveryTarget','-ExpectFailure')}
  @{n='connector-link-surface-adopt';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitSurfaceAdopt','-ExpectFailure')}
  @{n='connector-link-detached-mode';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitDetachedMode','-ExpectFailure')}
  @{n='connector-link-optc-term';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-DropOptcTerm','-ExpectFailure')}
  @{n='connector-link-rate-limit';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitRateLimit','-ExpectFailure')}
  @{n='connector-link-dto-check';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitDtoCheck','-ExpectFailure')}
  @{n='connector-link-inherited-target';s='driver\kmd\test\run_connector_link.ps1';a=@('-Kits',$Kits,'-OmitInheritedTarget','-ExpectFailure')}
  @{n='dml201';s='driver\shim\test\run_dml201.ps1';a=@('-Kits',$Kits)}
  @{n='dcn201-pipe';s='driver\shim\test\run_dcn201_pipe.ps1';a=@('-Kits',$Kits)}
  @{n='dpaudio-start-debt';s='driver\kmd\test\run_dpaudio_debt.ps1';a=@('-Root',$Workspace,'-Kits',$Kits)}
  @{n='wave-recovery-pending-control';s='driver\kmd\test\run_wave_recovery.ps1';a=@('-Kits',$Kits,'-NoPendingMarker');Fails=$true}
  @{n='wave-recovery-order-control';s='driver\kmd\test\run_wave_recovery.ps1';a=@('-Kits',$Kits,'-ClearBeforeData');Fails=$true}
  @{n='wave-recovery-delete-control';s='driver\kmd\test\run_wave_recovery.ps1';a=@('-Kits',$Kits,'-AdminReadOnly');Fails=$true}
  @{n='display-visibility';s='driver\kmd\test\run_display_visibility.ps1';a=@('-Root',$Workspace)}
  @{n='post-display-stop';s='driver\kmd\test\run_post_display_stop.ps1';a=@('-Root',$Workspace)}
  @{n='post-display-stop-no-handoff';s='driver\kmd\test\run_post_display_stop.ps1';a=@('-Root',$Workspace,'-SkipHandoff');Fails=$true}
  @{n='scanout-geometry';s='driver\kmd\test\run_scanout_geometry.ps1';a=@('-Root',$Workspace)}
  @{n='post-display';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace)}
  @{n='post-display-old-runtime-order';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace,'-OldRuntimeOrder');Fails=$true}
  @{n='post-display-wrong-saved-surface';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace,'-WrongSavedSurface');Fails=$true}
  @{n='post-display-resume-refusal';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace,'-ResumeRefusalIgnored');Fails=$true}
  @{n='post-display-application-surface';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace,'-SavedApplicationSurface');Fails=$true}
  # BD-090 (31 line): the firmware's HUBP_TTU_DISABLE is not a blank of this driver's own, and no DCN write names a
  # register that MmioDcnWriteEx refuses (the write-site check runs its own positive and negative controls first).
  @{n='post-display-ttu-means-blank';s='driver\kmd\test\run_post_display.ps1';a=@('-Root',$Workspace,'-TtuMeansBlank');Fails=$true}
  @{n='dcn-write-sites';s='driver\kmd\test\run_dcn_write_sites.ps1';a=@('-Root',$Workspace)}
  @{n='dcn-flip';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace)}
  @{n='dcn-flip-pending-only';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','PendingOnly');Fails=$true}
  @{n='dcn-flip-address-only';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','AddressOnly');Fails=$true}
  @{n='dcn-flip-bound-at-start';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BoundAtStart');Fails=$true}
  @{n='dcn-flip-overrun-uncounted';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','OverrunUncounted');Fails=$true}
  @{n='dcn-flip-poll-after-stall';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','PollAfterStall');Fails=$true}
  @{n='dcn-flip-pending-arm';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','PendingArmIgnored');Fails=$true}
  @{n='dcn-flip-logger-under-lock';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','SourceAddressLogs');Fails=$true}
  @{n='dcn-flip-bridge-proof';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeProofIgnored');Fails=$true}
  @{n='dcn-flip-bridge-extent';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeExtentIgnored');Fails=$true}
  @{n='dcn-flip-bridge-current';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeCurrentUnsafe');Fails=$true}
  @{n='dcn-flip-retire-stall';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeRetirePostStall');Fails=$true}
  @{n='dcn-flip-retire-deadline';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeRetirePostDeadline');Fails=$true}
  @{n='dcn-flip-retire-irql';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeRetireAtDirql');Fails=$true}
  @{n='dcn-flip-retire-frame';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeRetireOneFrame');Fails=$true}
  @{n='dcn-flip-bridge-adopt';s='driver\kmd\test\run_dcn_flip.ps1';a=@('-Root',$Workspace,'-Mutation','BridgeAdoptIgnored');Fails=$true}
  @{n='flip-state';s='driver\kmd\test\run_flip_state.ps1';a=@('-Root',$Workspace,'-Kits',$Kits)}
  # Its deadline is 600 s in gate-inputs.json: 22 s to 92.3 s on 2026-10-28, half of the old 180 s under load.
  @{n='flip-state-controls';s='driver\kmd\test\run_flip_state.ps1';a=@('-Root',$Workspace,'-Kits',$Kits,'-Mutation','all')}
  @{n='dcn-observation';s='driver\kmd\test\run_dcn_observation.ps1';a=@('-Root',$Workspace)}
  @{n='dcn-observe';s='driver\kmd\test\run_dcn_observe.ps1';a=@('-Root',$Workspace)}
  @{n='display-timing';s='driver\kmd\test\run_display_timing.ps1';a=@('-Root',$Workspace)}
  @{n='display-timing-old-describe';s='driver\kmd\test\run_display_timing.ps1';a=@('-Root',$Workspace,'-OldDescribe60');Fails=$true}
  @{n='display-timing-phase-hz';s='driver\kmd\test\run_display_timing.ps1';a=@('-Root',$Workspace,'-AssumePhaseHz');Fails=$true}
  @{n='display-timing-old-power-return';s='driver\kmd\test\run_display_timing.ps1';a=@('-Root',$Workspace,'-OldPowerReturn');Fails=$true}
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
  @{n='start-visibility-no-initial-blank';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-NoInitialBlank');Fails=$true}
  @{n='start-visibility-no-final-blank';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-NoFinalBlank');Fails=$true}
  @{n='start-visibility-stop-keeps-hold';s='driver\kmd\test\run_start_record.ps1';a=@('-Root',$Workspace,'-KeepStopVisibilityHold');Fails=$true}
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
  # TDR recovery S1: the adapter reset adopts the close of a ResetEngine refusal before it, and no fault.
  @{n='start-health-adapter-reset-ignores-refusal';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-AdapterResetIgnoresRefusal');Fails=$true}
  @{n='start-health-fault-keeps-refusal';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-FaultKeepsRefusal');Fails=$true}
  @{n='start-health-begin-keeps-refusal';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-BeginKeepsRefusal');Fails=$true}
  @{n='start-health-resume-keeps-refusal';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-ResumeKeepsRefusal');Fails=$true}
  @{n='start-health-refusal-over-failed-engines';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-RefusalOverFailedEngines');Fails=$true}
  @{n='start-health-refusal-over-closed-witness';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-RefusalOverClosedWitness');Fails=$true}
  # QUIET (0.7.216.104): an idle GPU desktop confirms with nothing owed, and a flip still owed never confirms.
  @{n='start-health-quiet-ignores-owed-flip';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-QuietIgnoresOwedFlip');Fails=$true}
  @{n='start-health-no-quiet';s='driver\kmd\test\run_start_health.ps1';a=@('-Root',$Workspace,'-NoQuiet');Fails=$true}
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
  @{n='adapter-reset';s='driver\kmd\test\run_adapter_reset.ps1';a=@('-Root',$Workspace)}
  @{n='adapter-reset-last-sample-only';s='driver\kmd\test\run_adapter_reset.ps1';a=@('-Root',$Workspace,'-LastSampleOnly');Fails=$true}
  @{n='adapter-reset-no-sdma-idle';s='driver\kmd\test\run_adapter_reset.ps1';a=@('-Root',$Workspace,'-IgnoreSdmaIdle');Fails=$true}
  @{n='adapter-reset-partial-begin';s='driver\kmd\test\run_adapter_reset.ps1';a=@('-Root',$Workspace,'-PartialBegin');Fails=$true}
  @{n='adapter-reset-complete-window';s='driver\kmd\test\run_adapter_reset.ps1';a=@('-Root',$Workspace,'-CompleteNonEmptyWindow');Fails=$true}
  @{n='adapter-reset-witness-order';s='driver\kmd\test\run_adapter_reset.ps1';a=@('-Root',$Workspace,'-ReorderWitnessTable');Fails=$true}
  @{n='adapter-reset-commit-first';s='driver\kmd\test\run_adapter_reset.ps1';a=@('-Root',$Workspace,'-CommitBeforeWitness');Fails=$true}
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
  @{n='recovery-protocol-RefusalIgnoresAdapterRecovery';s='driver\kmd\test\run_recovery_protocol.ps1';a=@('-Root',$Workspace,'-RefusalIgnoresAdapterRecovery');Fails=$true}
  @{n='submit-watchdog-flat-500';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-FlatFiveHundred');Fails=$true}
  @{n='submit-watchdog-short-diagnostic';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-AllowTooShortDiagnostic');Fails=$true}
  @{n='submit-watchdog-long-diagnostic';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-AllowTooLongDiagnostic');Fails=$true}
  @{n='submit-watchdog-ignore-progress';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-IgnoreProgress');Fails=$true}
  @{n='submit-watchdog-stamp-at-ring-write';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-StampAtRingWrite');Fails=$true}
  @{n='submit-watchdog-report-with-pending';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-ReportWithPendingCompletion');Fails=$true}
  @{n='submit-watchdog-no-fence-range';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-NoFenceRange');Fails=$true}
  @{n='submit-watchdog-log-the-constant';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-LogTheConstant');Fails=$true}
  @{n='submit-watchdog-rearm-outside-lock';s='driver\kmd\test\run_submit_watchdog.ps1';a=@('-Root',$Workspace,'-RearmOutsideLock');Fails=$true}
  @{n='vmid-pool-ignore-retirement';s='driver\kmd\test\run_vmid_pool.ps1';a=@('-Root',$Workspace,'-IgnoreRetirement');Fails=$true}
  @{n='vmid-pool-ignore-compute-ring';s='driver\kmd\test\run_vmid_pool.ps1';a=@('-Root',$Workspace,'-IgnoreComputeRing');Fails=$true}
  # The gfx-pipeline controls check their own pinned CHECK failure counts (run_gfx_pipeline.ps1) and exit 0 when the
  # control failed as required; a crash, a deadline or a build error fails them.
  @{n='gfx-pipeline-idle-only';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-IdleOnlyPresent')}
  @{n='gfx-pipeline-no-capacity';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-WithoutCapacity')}
  @{n='gfx-pipeline-equal-fence';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-EqualityFence')}
  @{n='gfx-pipeline-no-boundary-pad';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-WithoutBoundaryPad')}
  @{n='gfx-pipeline-no-pad-capacity';s='driver\kmd\test\run_gfx_pipeline.ps1';a=@('-Root',$Workspace,'-KmdOnly','-WithoutPadCapacity')}
  # -Root as well: this runner puts the compiler's TEMP under it, and a worktree's parent has no scratch\tmp.
  @{n='firmware-metadata-no-runtime';s='driver\kmd\test\run_firmware_metadata.ps1';a=@('-Root',$Workspace,'-Kits',$Kits,'-Firmware',$firmware,'-OmitRuntimeFirmware');Fails=$true}
  @{n='firmware-io';s='driver\kmd\test\run_firmware_io.ps1';a=@('-Kits',$Kits)}
  @{n='firmware-io-short-read';s='driver\kmd\test\run_firmware_io.ps1';a=@('-Kits',$Kits,'-AcceptShort');Fails=$true}
  @{n='paging-mc';s='driver\kmd\test\run_paging_mc.ps1';a=@('-Kits',$Kits)}
  @{n='paging-private';s='driver\kmd\test\run_paging_private.ps1';a=@('-Kits',$Kits)}
  @{n='paging-private-slot-reuse';s='driver\kmd\test\run_paging_private.ps1';a=@('-Kits',$Kits,'-ReuseFirstQueueSlot');Fails=$true}
  @{n='paging-pt-shadow';s='driver\kmd\test\run_paging_pt_shadow.ps1';a=@('-Kits',$Kits)}
  @{n='paging-stream';s='driver\kmd\test\run_paging_stream.ps1';a=@('-Kits',$Kits)}
  @{n='paging-window';s='driver\kmd\test\run_paging_window.ps1';a=@('-Kits',$Kits)}
  @{n='shim-replay';s='driver\shim\test\run.ps1';a=@('-Kits',$Kits)}
  @{n='shim-gfx';s='driver\shim\test\run_gfx.ps1';a=@('-Kits',$Kits)}
  # The frozen gfx job frame and the VM flush on the ring (docs/design/gfx-submit-root-serialization.md).
  @{n='gfx-vm-flush';s='driver\shim\test\run_gfx_vm_flush.ps1';a=@('-Root',$Workspace,'-Kits',$Kits)}
  @{n='gfx-vm-flush-wrong-ack';s='driver\shim\test\run_gfx_vm_flush.ps1';a=@('-Root',$Workspace,'-Kits',$Kits,'-WrongAckMask');Fails=$true}
  @{n='shim-ih';s='driver\shim\test\run_ih.ps1';a=@('-Kits',$Kits)}
  @{n='shim-psp';s='driver\shim\test\run_psp.ps1';a=@('-Kits',$Kits,'-Firmware',$firmware)}
  @{n='shim-pte';s='driver\shim\test\run_pte.ps1';a=@('-Kits',$Kits)}
  @{n='shim-paging';s='driver\shim\test\run_paging.ps1';a=@('-Kits',$Kits)}
  @{n='sdma-copy';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$Kits)}
  @{n='sdma-copy-vmid-zero';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$Kits,'-ForceIbVmidZero');Fails=$true}
  @{n='sdma-copy-no-vm-flush';s='driver\shim\test\run_sdma_copy.ps1';a=@('-Kits',$Kits,'-OmitVmFlush');Fails=$true}
  @{n='sdma-faults';s='driver\shim\test\run_sdma_faults.ps1';a=@('-Kits',$Kits)}
  @{n='smu-mailbox';s='driver\shim\test\run_smu.ps1';a=@('-Root',$Workspace)}
  @{n='board-sequence';s='driver\shim\test\run_board_sequence.ps1';a=@('-Root',$Workspace)}
  @{n='board-sequence-changed-point';s='driver\shim\test\run_board_sequence.ps1';a=@('-Root',$Workspace,'-Mutation','gpu-point');Fails=$true}
  @{n='smu-native-board-admission';s='driver\shim\test\run_smu_native.ps1';a=@('-Root',$Workspace,'-Mutation','board-admission');Fails=$true}
  # The clock escape's two gates (BD-062): the token in front of the write, and the published reading the open
  # READ rests on. Each mutation removes one of them, and the suite must fail by a check; a mutation that nobody
  # runs is not a control, so both are listed here and not only in the runner.
  @{n='smu-native-clock-write-privilege';s='driver\shim\test\run_smu_native.ps1';a=@('-Root',$Workspace,'-Mutation','clock-write-privilege');Fails=$true}
  @{n='smu-native-clock-read-snapshot';s='driver\shim\test\run_smu_native.ps1';a=@('-Root',$Workspace,'-Mutation','clock-read-snapshot');Fails=$true}
  @{n='hwmon';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits)}
  @{n='hwmon-board-port';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits,'-Mutation','board-port');Fails=$true}
  # The case fan's load feed-forward (rule 10 of driver\shim\include\bc250_fan.h): the first negative controls of
  # the fan suite, because this is the rule that writes 255 to the chip by itself. Each -Mutation changes one line
  # of a copy of the file under test and the suite must then fail by a check.
  @{n='hwmon-no-boost-raise';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits,'-Mutation','no-boost-raise');Fails=$true}
  @{n='hwmon-boost-held-back';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits,'-Mutation','boost-held-back');Fails=$true}
  @{n='hwmon-step-pays-all';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits,'-Mutation','step-pays-all');Fails=$true}
  @{n='hwmon-telemetry-always';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits,'-Mutation','telemetry-always');Fails=$true}
  @{n='hwmon-boost-claims-fixed';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits,'-Mutation','boost-claims-fixed');Fails=$true}
  @{n='hwmon-rise-outlives-doubt';s='driver\shim\test\run_hwmon.ps1';a=@('-Kits',$Kits,'-Mutation','rise-outlives-doubt');Fails=$true}
  @{n='smu-metrics';s='driver\shim\test\run_smu_metrics.ps1';a=@('-Kits',$Kits)}
  @{n='contract-caps';s='driver\contract\test\run.ps1';a=@('-Kits',$Kits,'-Mesa',"$Workspace\ref\mesa")}
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
