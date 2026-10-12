# Host unit test of the engine parts that the setup window relies on (GUI plan A1, C7, F-VER), Windows PowerShell 5.1
# like the installer:
#   powershell -NoProfile -File tools\release\test-engine-units.ps1 [-Installer <package>\installer] [-WorkRoot <dir>]
# G-STAGE: the RunOnce command line (Format-CommandLine, parsed back through CommandLineToArgvW), the continuation
# closure (staged once, checked, a changed copy repaired, a damaged source refused), the continuation command of the
# setup window and of the command line (run for real from folders with spaces and &, ^, %, ( ), !, ;), and the kept
# repair set (active + previous, older removed, firmware completed, each set against its own manifest's firmware).
# Witness writer (G-VER, writer side): the record binds the loaded image, the reply and the boot; the reader's rule;
# publication under engine.lock with a bounded wait, and no witness when an install action appears during the reading.
# Compatibility record (C7, used by G-RB from Ph 3): a compliant record verifies; the tester.10 shape (no Reboot
# directive), a missing or unknown record, null, scalar, array or empty records, another firmware set, an incomplete
# firmware folder and another engine contract are refused. The engine lock: one mutating engine at a time; a first
# change whose record cannot be saved does not run. Pending restarts (Get-PendingRestart). Child closure from the
# job's own count (this process joins a kill-on-close job in the last section).
# GitHub #38: the start state verify reads before it waits for the start-confirm task and before it probes the
# adapter (Get-VerifyStartState), and install.ps1's verify holding to it: a failed start skips the wait and the
# probes that need a started adapter, records each of them, and still writes its report.
# BD-125: Secure Boot from the UEFI variable with stand-in readers (no variable = off, not built into the firmware;
# the fallbacks answer only when the read fails), one real read of the variable on this host, and the certificate
# step's error (store, HRESULT, certutil commands), also through the shipped step with a stand-in Import-Certificate.
# Everything is written under -WorkRoot; nothing in the registry, no driver, no restart.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$WorkRoot)
# Default scratch: beside the folder the zip unpacks to, never inside it. The package root is files\ of that folder
# (owner, 2026-10-11), so one more level up than it used to be; an older flat package keeps the old place.
if (-not $PSBoundParameters.ContainsKey('WorkRoot')) {
    $packageRoot = Split-Path $Installer   # <package>\installer -> <package>
    $packageTop = $(if ((Split-Path $packageRoot -Leaf) -eq 'files') { Split-Path $packageRoot } else { $packageRoot })
    $WorkRoot = Join-Path (Split-Path $packageTop) 'test-tmp'
}
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
. (Join-Path $Installer 'engine.ps1')
. (Join-Path $Installer 'release-witness.ps1')
. (Join-Path $Installer 'compatibility.ps1')
$script:ScheduleOldCopies = $false
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
$work = Join-Path $WorkRoot ('units-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
[void][IO.Directory]::CreateDirectory($work)
$script:StateDir = Join-Path $work 'state'
$script:StatePath = Join-Path $script:StateDir 'state.json'
function Write-Text([string]$Path, [string]$Text) { [void][IO.Directory]::CreateDirectory((Split-Path $Path)); [IO.File]::WriteAllText($Path, $Text) }
function Get-TextSha([string]$Text) { return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))) -replace '-', '') }

# A small package: the files the record and the closure read, and a manifest.json over all of them.
$fwFiles = @(
    [pscustomobject]@{ name = 'fw_a.bin'; path = 'amdgpu/fw_a.bin'; text = 'firmware a' }
    [pscustomobject]@{ name = 'LICENSE.amdgpu'; path = 'LICENSE.amdgpu'; text = 'licence' })
$fwPin = [pscustomobject]@{ commit = ('0' * 40); install_dir = 'C:\BC250\firmware'; url_templates = @('https://example.invalid/{path}?id={commit}')
    files = @($fwFiles | ForEach-Object { [pscustomobject]@{ name = $_.name; path = $_.path; sha256 = (Get-TextSha $_.text); size = $_.text.Length } }) }
$infGood = "[Version]`r`nSignature = `"`$Windows NT`$`"`r`n`r`n[Manufacturer]`r`n%P% = Models,NTamd64`r`n`r`n[Models.NTamd64]`r`n%D% = Bc250_Install, PCI\VEN_1002&DEV_13FE`r`n`r`n[Bc250_Install]`r`nReboot`r`nCopyFiles = F`r`n"
$infTester10 = $infGood -replace "Reboot`r`n", ''
function New-FakePackage {
    param([string]$Dir, [string]$Version = '0.7.199.100-test.1', [string]$Inf = $infGood, [switch]$NoRecord, [scriptblock]$EditRecord, $Firmware = $fwPin, [AllowNull()][AllowEmptyString()][string]$RecordText = $null, [switch]$RawRecord, [switch]$Setup)
    Write-Text (Join-Path $Dir 'payload\kmd\bc250kmd.inf') $Inf
    Write-Text (Join-Path $Dir 'payload\kmd\bc250kmd.sys') "kmd image $Version"
    Write-Text (Join-Path $Dir 'installer\registry-defaults.json') '{ "schema": 1, "defaults": { "parameters": { "DpmMode": 1 }, "desktop_router": { "DwmForceCpu": 0 } } }'
    Write-Text (Join-Path $Dir 'install.cmd') '@echo off'
    # -Setup: the unpacked release's shape, setup.exe in the top folder beside the package root files\.
    if ($Setup) { Write-Text (Join-Path (Split-Path -Parent $Dir.TrimEnd('\')) 'setup.exe') "setup window $Version" }
    if ($RawRecord) { Write-Text (Join-Path $Dir 'compatibility.json') $RecordText }
    elseif (-not $NoRecord) {
        $rec = New-CompatibilityRecord -PackageRoot $Dir -Version $Version -Firmware $Firmware -KmdBuild '0.7.199.1' -KmdAbi '0x000700C7' -DriverVer '0.7.199.100'
        if ($EditRecord) { & $EditRecord $rec }
        Write-Text (Join-Path $Dir 'compatibility.json') ($rec | ConvertTo-Json -Depth 6)
    }
    $files = @(Get-ChildItem -LiteralPath $Dir -Recurse -File | Where-Object { $_.Name -ne 'manifest.json' } | Sort-Object FullName | ForEach-Object {
            [ordered]@{ path = ($_.FullName.Substring($Dir.TrimEnd('\').Length + 1) -replace '\\', '/'); sha256 = (Get-Sha256 $_.FullName); size = $_.Length } })
    $m = [ordered]@{ schema = 1; name = "pkg-$Version"; release = $Version; version = $Version; kmd_version = '0.7.199.100'; kmd_build = '0.7.199.1'; kmd_abi = '0x000700C7'
        firmware = $fwPin; components = @([ordered]@{ role = 'kmd'; package_path = 'payload/kmd/bc250kmd.sys'; sha256 = (Get-Sha256 (Join-Path $Dir 'payload\kmd\bc250kmd.sys')) }); files = $files }
    if ($Setup) {
        $se = Join-Path (Split-Path -Parent $Dir.TrimEnd('\')) 'setup.exe'
        $m.setup = [ordered]@{ file = 'setup.exe'; sha256 = (Get-Sha256 $se); size = (Get-Item -LiteralPath $se).Length }
    }
    Write-Text (Join-Path $Dir 'manifest.json') ($m | ConvertTo-Json -Depth 6)
    return (Get-Content -LiteralPath (Join-Path $Dir 'manifest.json') -Raw | ConvertFrom-Json)
}
$fwDir = Join-Path $work 'firmware-good'
foreach ($f in $fwFiles) { Write-Text (Join-Path $fwDir $f.name) $f.text }

'[G-STAGE] RunOnce command line: each part quoted on its own, parsed back unchanged'
if (-not ('EngineUnits.Argv' -as [type])) {
    Add-Type -Namespace EngineUnits -Name Argv -MemberDefinition @'
[DllImport("shell32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CommandLineToArgvW(string cmd, out int argc);
[DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
public static string[] Split(string cmd) {
    int n; IntPtr p = CommandLineToArgvW(cmd, out n);
    var r = new string[n];
    for (int i = 0; i < n; i++) r[i] = System.Runtime.InteropServices.Marshal.PtrToStringUni(System.Runtime.InteropServices.Marshal.ReadIntPtr(p, i * IntPtr.Size));
    LocalFree(p); return r;
}
'@
}
foreach ($c in @(
        @{ exe = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'; args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'C:\ProgramData\amdgpu-wddm\installer\packages\0.7.199.100-tester.12\installer\install.ps1', '-HoldWindow') }
        @{ exe = 'C:\Program Data\x y\setup\amdgpu_wddm_setup.exe'; args = @('--continue') }
        @{ exe = 'C:\a\setup.exe'; args = @('--package', 'D:\my folder\pkg', '') }
        @{ exe = 'C:\AMD&GPU\setup.exe'; args = @('-File', 'C:\AMD&GPU\installer\install.ps1', 'C:\a (x)\b^c%PATH%!d', 'C:\trailing space\', 'C:\x&y\\') })) {
    $line = Format-CommandLine $c.exe $c.args
    $back = [EngineUnits.Argv]::Split($line)
    $want = @($c.exe) + @($c.args)
    Check ((($back -join '|') -eq ($want -join '|')) -and ($line.StartsWith('"' + $c.exe + '" '))) "round trip: $line"
}
Check ((Format-CommandLine 'C:\x.exe' @('C:\AMD&GPU\a.ps1')) -eq '"C:\x.exe" "C:\AMD&GPU\a.ps1"') 'an argument with & is quoted'
$threw = $false; try { [void](Format-CommandLine 'C:\a.exe' @('x"y')) } catch { $threw = $true }
Check $threw 'an argument with a quote is refused'

'[G-STAGE] continuation closure: staged once and checked, idempotent, a changed copy repaired, a damaged source refused'
$pkg = Join-Path $work 'pkg'
$m = New-FakePackage -Dir $pkg
$closure = Get-ClosureDir ([string]$m.version)
Check ($closure -eq (Join-Path $script:StateDir "packages\$($m.version)")) "closure path $closure"
$r1 = Save-ContinuationClosure -PackageRoot $pkg -Manifest $m -FirmwareDir $fwDir
Check (($r1 -eq $closure) -and (Test-PackageManifest -PackageRoot $closure).ok -and -not (Test-FirmwareFolder $m.firmware (Join-Path $closure 'firmware')).Count) 'first staging: every file and the firmware checked in the closure'
$t1 = (Get-Item -LiteralPath (Join-Path $closure 'install.cmd')).LastWriteTimeUtc
Start-Sleep -Milliseconds 50
[void](Save-ContinuationClosure -PackageRoot $pkg -Manifest $m -FirmwareDir $fwDir)
Check ((Get-Item -LiteralPath (Join-Path $closure 'install.cmd')).LastWriteTimeUtc -eq $t1) 'second staging: files with the same SHA256 are kept'
[IO.File]::AppendAllText((Join-Path $closure 'install.cmd'), 'tampered')
[IO.File]::AppendAllText((Join-Path $closure 'firmware\fw_a.bin'), 'tampered')
[void](Save-ContinuationClosure -PackageRoot $pkg -Manifest $m -FirmwareDir $fwDir)
Check ((Test-PackageManifest -PackageRoot $closure).ok -and -not (Test-FirmwareFolder $m.firmware (Join-Path $closure 'firmware')).Count) 'a changed copy in the closure is replaced and checked again'
[void](Save-ContinuationClosure -PackageRoot $closure -Manifest $m)
Check (Test-PackageManifest -PackageRoot $closure).ok 'a run that starts from its closure only checks it'
$bad = Join-Path $work 'pkg-damaged'
$mb = New-FakePackage -Dir $bad -Version '0.7.199.100-test.2'
[IO.File]::AppendAllText((Join-Path $bad 'payload\kmd\bc250kmd.inf'), '; changed')
$threw = $null; try { [void](Save-ContinuationClosure -PackageRoot $bad -Manifest $mb) } catch { $threw = $_.Exception.Message }
Check ($threw -match 'continuation closure .*changed payload/kmd/bc250kmd\.inf') "a damaged source package is refused: $threw"
$badFw = Join-Path $work 'firmware-bad'
Write-Text (Join-Path $badFw 'fw_a.bin') 'other'
$threw = $null; try { [void](Save-ContinuationClosure -PackageRoot $pkg -Manifest $m -FirmwareDir $badFw -Destination (Join-Path $work 'closure-badfw')) } catch { $threw = $_.Exception.Message }
Check ($threw -match 'firmware') "a firmware folder with a changed or missing file is refused: $threw"

'[G-STAGE] package layout: setup.exe beside files\ in the unpacked release, in the folder itself in a closure, hashed by the manifest'
$top = Join-Path $work 'unpacked\amdgpu-wddm-tester-test.3'
$lay = Join-Path $top 'files'
$mL = New-FakePackage -Dir $lay -Version '0.7.199.100-test.3' -Setup
Check ((Get-PackageSetupExe $lay) -eq (Join-Path $top 'setup.exe')) 'the setup window of an unpacked release is found one level above the package root'
Check (Test-PackageManifest -PackageRoot $lay).ok 'the package verifies with setup.exe beside files\'
$lc = Save-ContinuationClosure -PackageRoot $lay -Manifest $mL -FirmwareDir $fwDir
Check (Test-Path -LiteralPath (Join-Path $lc 'setup.exe') -PathType Leaf) 'the closure gets its own copy of setup.exe'
Check ((Get-PackageSetupExe $lc) -eq (Join-Path $lc 'setup.exe')) 'the setup window of a closure is found in the package root itself'
Check (Test-PackageManifest -PackageRoot $lc).ok 'the closure verifies with its own copy'
[IO.File]::AppendAllText((Join-Path $lc 'setup.exe'), 'tampered')
$chk = Test-PackageManifest -PackageRoot $lc
Check ((-not $chk.ok) -and ($chk.detail -match 'changed setup\.exe')) "a changed setup.exe is a finding: $($chk.detail)"
Remove-Item -LiteralPath (Join-Path $lc 'setup.exe') -Force
$chk = Test-PackageManifest -PackageRoot $lc
Check ((-not $chk.ok) -and ($chk.detail -match 'missing setup\.exe')) "a missing setup.exe is a finding: $($chk.detail)"
[void](Save-ContinuationClosure -PackageRoot $lay -Manifest $mL -FirmwareDir $fwDir)
Check (Test-PackageManifest -PackageRoot $lc).ok 'a closure whose setup.exe was removed is staged again and verifies'
# A folder that holds its own copy never reads the folder above it: a setup.exe of another build beside a closure or
# a prepared folder must not hide the package's own copy, and must not stand in for a tampered one.
$foreign = Join-Path (Split-Path $lc) 'setup.exe'
Write-Text $foreign 'OTHER BUILD'
Check ((Get-PackageSetupExe $lc) -eq (Join-Path $lc 'setup.exe')) 'a closure reads its own setup.exe, not another build''s copy beside it'
Check (Test-PackageManifest -PackageRoot $lc).ok 'a closure with its own copy verifies although the folder above holds another setup.exe'
[IO.File]::AppendAllText((Join-Path $lc 'setup.exe'), 'tampered')
$chk = Test-PackageManifest -PackageRoot $lc
Check ((-not $chk.ok) -and ($chk.detail -match 'changed setup\.exe')) "a tampered copy is not masked by the folder above: $($chk.detail)"
Remove-Item -LiteralPath $foreign -Force
[void](Save-ContinuationClosure -PackageRoot $lay -Manifest $mL -FirmwareDir $fwDir)
$recL = New-CompatibilityRecord -PackageRoot $lay -Version '0.7.199.100-test.3' -Firmware $fwPin -KmdBuild '0.7.199.1' -KmdAbi '0x000700C7' -DriverVer '0.7.199.100'
Check ($recL.continuation.setup_exe -eq 'setup.exe') "the compatibility record names the setup window by its file name: $($recL.continuation.setup_exe)"

'[G-STAGE] continuation command: the setup window of the closure, else Windows PowerShell with the closure''s install.ps1; verify from the install root'
$psExe = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$c = Get-ContinuationCommand -Kind continue -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $true
Check (($c.exe -eq $psExe) -and ((@($c.arguments) -join ' ') -eq "-NoProfile -ExecutionPolicy Bypass -File $closure\installer\install.ps1 -HoldWindow")) 'GUI run, closure without a setup window: Windows PowerShell runs the closure''s install.ps1 directly (no cmd.exe)'
Write-Text (Join-Path $closure $script:SetupExeName) 'exe'
$c = Get-ContinuationCommand -Kind continue -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $true
Check (($c.exe -eq (Join-Path $closure $script:SetupExeName)) -and ((@($c.arguments) -join ' ') -eq '--continue')) 'GUI run: the closure''s setup window with --continue'
$c = Get-ContinuationCommand -Kind continue -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $false
Check ((@($c.arguments) -join ' ') -eq "-NoProfile -ExecutionPolicy Bypass -File $closure\installer\install.ps1 -HoldWindow") 'command-line run: the closure''s install.ps1, never the original folder'
$c = Get-ContinuationCommand -Kind verify -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $false
$line = Format-CommandLine $c.exe $c.arguments
$argv = [EngineUnits.Argv]::Split($line)
Check (($argv[0] -eq $psExe) -and ($argv[5] -eq 'C:\Program Files\amdgpu-wddm\installer\install.ps1') -and (($argv[6..7] -join ' ') -eq '-HoldWindow -Verify')) "verify: $line"
Remove-Item -LiteralPath (Join-Path $closure $script:SetupExeName) -Force

'[G-STAGE] RunOnce continuation run for real from folders with spaces and command metacharacters (R7)'
# The RunOnce line is started as Windows starts it (program, then the tail as one string); a stub install.ps1 records
# the path and the switches it received. No console window: CreateNoWindow.
foreach ($name in 'AMD&GPU', 'a b (x) 100%PATH%^!y', 'semi;colon,comma=eq') {
    $root = Join-Path $work "runonce\$name"
    Write-Text (Join-Path $root 'installer\install.ps1') "param([switch]`$HoldWindow, [switch]`$Verify)`r`n[IO.File]::WriteAllText((Join-Path `$PSScriptRoot 'ran.json'), ([ordered]@{ path = `$PSCommandPath; hold = [bool]`$HoldWindow; verify = [bool]`$Verify } | ConvertTo-Json))`r`n"
    foreach ($kind in 'continue', 'verify') {
        $ran = Join-Path $root 'installer\ran.json'
        Remove-Item -LiteralPath $ran -Force -ErrorAction SilentlyContinue
        $c = Get-ContinuationCommand -Kind $kind -Closure $root -InstallRoot $root -Gui $false
        $line = Format-CommandLine $c.exe $c.arguments
        $tail = $line.Substring($c.exe.Length + 3)
        $psi = New-Object Diagnostics.ProcessStartInfo -ArgumentList $c.exe, $tail
        $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
        $p = [Diagnostics.Process]::Start($psi)
        [void]$p.WaitForExit(60000)
        $got = $(if (Test-Path -LiteralPath $ran) { Get-Content -LiteralPath $ran -Raw | ConvertFrom-Json })
        Check ($got -and ($got.path -eq (Join-Path $root 'installer\install.ps1')) -and $got.hold -and ($got.verify -eq ($kind -eq 'verify'))) "${kind} from '$name': the script ran with its own path and switches ($line)"
    }
}

'[G-STAGE] kept repair set: active + previous, older removed, firmware completed from the installed files'
$pk = Get-PackagesDir
foreach ($v in '0.7.197.100-tester.1', '0.7.198.100-tester.10') { Write-Text (Join-Path $pk "$v\manifest.json") '{}' }
Write-Text (Join-Path $pk 'index.json') (([ordered]@{ schema = $script:RepairIndexSchema; active = [ordered]@{ version = '0.7.198.100-tester.10' }; previous = [ordered]@{ version = '0.7.197.100-tester.1' } }) | ConvertTo-Json)
Remove-Item -LiteralPath (Join-Path $closure 'firmware') -Recurse -Force
$installed = Join-Path $work 'installed-firmware'
foreach ($f in $fwFiles) { Write-Text (Join-Path $installed $f.name) $f.text }
$idx = Complete-RepairSet -Manifest $m -Closure $closure -FirmwareFiles @($fwFiles | ForEach-Object { Join-Path $installed $_.name })
$left = @(Get-ChildItem -LiteralPath $pk -Directory | ForEach-Object { $_.Name } | Sort-Object)
Check ((($left -join ',') -eq "0.7.198.100-tester.10,$($m.version)") -and ($idx.active.version -eq $m.version) -and ($idx.previous.version -eq '0.7.198.100-tester.10')) "sets kept: $($left -join ', ')"
Check ($idx.active.firmware_complete -and -not $idx.previous.firmware_complete -and $idx.active.manifest_sha256 -eq (Get-Sha256 (Join-Path $closure 'manifest.json'))) 'index: active set complete with its manifest hash; the old set without firmware is marked incomplete'
$disk = Get-Content -LiteralPath (Join-Path $pk 'index.json') -Raw | ConvertFrom-Json
Check (($disk.schema -eq $script:RepairIndexSchema) -and ($disk.active.version -eq $m.version)) 'index.json written'
$p = Get-RepairSetPlan $disk $m.version @($m.version, '0.7.198.100-tester.10')
Check (($p.previous -eq '0.7.198.100-tester.10') -and -not @($p.remove).Count) 'a repair of the active version keeps the previous set'
$p = Get-RepairSetPlan $disk '0.7.200.100-tester.13' @($m.version, '0.7.198.100-tester.10', '0.7.200.100-tester.13')
Check (($p.previous -eq $m.version) -and ((@($p.remove) -join ',') -eq '0.7.198.100-tester.10')) 'an upgrade keeps the set it replaces as previous and removes the one before'

'[G-STAGE] kept repair set: each set is checked against the firmware of its own manifest (R10)'
$saveStateDir = $script:StateDir
$script:StateDir = Join-Path $work 'state-r10'
$pk = Get-PackagesDir
$fw2Files = @([pscustomobject]@{ name = 'fw_a.bin'; path = 'amdgpu/fw_a.bin'; text = 'firmware a, older release' }, [pscustomobject]@{ name = 'LICENSE.amdgpu'; path = 'LICENSE.amdgpu'; text = 'licence' })
$fwPin2 = [pscustomobject]@{ commit = ('1' * 40); install_dir = 'C:\BC250\firmware'; url_templates = $fwPin.url_templates
    files = @($fw2Files | ForEach-Object { [pscustomobject]@{ name = $_.name; path = $_.path; sha256 = (Get-TextSha $_.text); size = $_.text.Length } }) }
function New-KeptSet([string]$Version, $Pin, $Files) {
    $d = Join-Path $pk $Version
    Write-Text (Join-Path $d 'manifest.json') (([ordered]@{ schema = 1; name = "pkg-$Version"; version = $Version; firmware = $Pin }) | ConvertTo-Json -Depth 6)
    foreach ($f in $Files) { Write-Text (Join-Path $d "firmware\$($f.name)") $f.text }
    return (Get-Content -LiteralPath (Join-Path $d 'manifest.json') -Raw | ConvertFrom-Json)
}
foreach ($c in @(
        @{ name = 'older set with its own, different firmware'; files = $fw2Files; want = $true }
        @{ name = 'older set holding the new release''s firmware instead of its own'; files = $fwFiles; want = $false })) {
    Remove-Item -LiteralPath $pk -Recurse -Force -ErrorAction SilentlyContinue
    [void](New-KeptSet '0.7.198.100-tester.10' $fwPin2 $c.files)
    $mNew = New-KeptSet '0.7.199.100-tester.11' $fwPin $fwFiles
    Write-Text (Join-Path $pk 'index.json') (([ordered]@{ schema = $script:RepairIndexSchema; active = [ordered]@{ version = '0.7.198.100-tester.10' } }) | ConvertTo-Json)
    $idx = Complete-RepairSet -Manifest $mNew -Closure (Join-Path $pk '0.7.199.100-tester.11')
    Check (($idx.previous.version -eq '0.7.198.100-tester.10') -and ($idx.previous.firmware_complete -eq $c.want) -and $idx.previous.manifest_valid -and $idx.active.firmware_complete) "$($c.name): previous firmware_complete $($idx.previous.firmware_complete) (want $($c.want)), active complete"
}
Write-Text (Join-Path $pk '0.7.198.100-tester.10\manifest.json') '{ "version": "0.7.197.100-tester.1" }'
$idx = Complete-RepairSet -Manifest $mNew -Closure (Join-Path $pk '0.7.199.100-tester.11')
Check ((-not $idx.previous.manifest_valid) -and (-not $idx.previous.firmware_complete)) 'a set whose manifest names another version or no firmware is not complete'
$script:StateDir = $saveStateDir

'[BD-089] the files of the installed release that a new one does not install, and our older driver packages'
$bdRoot = Join-Path $work 'bd089\install-root'
$bdSys = Join-Path $work 'bd089\system32'
$bdWow = Join-Path $work 'bd089\syswow64'
# A manifest of the shape the installer reads from disk: package-relative paths with the SHA256 of each file.
function New-Bd089Manifest {
    param([string]$Version, [string[]]$Paths, [hashtable]$Hashes = @{})
    $files = @($Paths | ForEach-Object { [ordered]@{ path = $_; sha256 = $(if ($Hashes.ContainsKey($_)) { $Hashes[$_] } else { Get-TextSha "content of $_" }); size = 1 } })
    return ((([ordered]@{ schema = 1; version = $Version; files = $files }) | ConvertTo-Json -Depth 5) | ConvertFrom-Json)
}
$mapCases = @(
    @{ p = 'payload/d3d12/amdgpu_wddm_d3d12.dll'; want = (Join-Path $bdRoot 'd3d12\amdgpu_wddm_d3d12.dll') }
    @{ p = 'payload/wow64/d3d11/amdgpu_wddm_d3d11.dll'; want = (Join-Path $bdRoot 'wow64\d3d11\amdgpu_wddm_d3d11.dll') }
    @{ p = 'payload/wow64/d3d12/amdgpu_wddm_d3d12.dll'; want = (Join-Path $bdRoot 'wow64\d3d12\amdgpu_wddm_d3d12.dll') }
    @{ p = 'payload/mft/amdgpu_wddm_mft_h264.dll'; want = (Join-Path $bdRoot 'mft\amdgpu_wddm_mft_h264.dll') }
    @{ p = 'payload/control/amdgpu_wddm_control.exe'; want = (Join-Path $bdRoot 'control\amdgpu_wddm_control.exe') }
    @{ p = 'payload/system32/bc250umd.dll'; want = (Join-Path $bdSys 'bc250umd.dll') }
    @{ p = 'payload/syswow64/bc250umd.dll'; want = (Join-Path $bdWow 'bc250umd.dll') }
    @{ p = 'installer/common.ps1'; want = (Join-Path $bdRoot 'installer\common.ps1') }
    @{ p = 'licenses/zlib-LICENSE.txt'; want = (Join-Path $bdRoot 'licenses\zlib-LICENSE.txt') }
    @{ p = 'THIRD-PARTY.md'; want = (Join-Path $bdRoot 'licenses\THIRD-PARTY.md') }
    @{ p = 'verify.cmd'; want = (Join-Path $bdRoot 'verify.cmd') }
    @{ p = 'uninstall.cmd'; want = (Join-Path $bdRoot 'uninstall.cmd') }
    @{ p = 'payload/kmd/bc250kmd.sys'; want = $null }                      # the driver store
    @{ p = 'payload/cert/amdgpu-wddm-release.cer'; want = $null }          # two certificate stores
    @{ p = 'payload/firmware/cyan_skillfish2_me.bin'; want = $null }       # the firmware folder of the KMD
    @{ p = 'payload/redist/vc_redist.x64.exe'; want = $null }              # Microsoft's installer runs from the package
    @{ p = 'install.cmd'; want = $null }                                   # read from the package, never copied
    @{ p = 'INSTALL.md'; want = $null }
    @{ p = 'setup.exe'; want = $null }                                     # the setup window runs from the package
    @{ p = 'payload/loose.dll'; want = $null }                             # no payload directory: nothing copies it
    @{ p = 'payload/d3d12/../../../evil.dll'; want = $null })
$bad = @()
foreach ($c in $mapCases) {
    $got = Get-InstalledPathOfPackageFile -PackagePath $c.p -InstallRoot $bdRoot -SystemDir $bdSys -SysWowDir $bdWow
    if ([string]$got -ne [string]$c.want) { $bad += "$($c.p) -> $(if ($got) { $got } else { 'nothing' }) (expected $(if ($c.want) { $c.want } else { 'nothing' }))" }
}
Check (-not $bad.Count) "$($mapCases.Count) package paths land where install.ps1 copies them$(if ($bad.Count) { ': ' + ($bad -join '; ') })"

$shared = @('payload/d3d12/keep.dll', 'payload/tools/keep.exe', 'installer/common.ps1', 'verify.cmd')
$prevManifest = New-Bd089Manifest -Version '0.7.212.100-tester.14' -Paths ($shared + @('payload/desktop/old_helper.dll', 'payload/tools/moved.exe', 'payload/kmd/bc250kmd.sys', 'install.cmd', 'payload/d3d12/../../../evil.dll'))
$newManifest = New-Bd089Manifest -Version '0.7.213.101-tester.16' -Paths ($shared + @('payload/d3d12/moved.exe', 'payload/d3d12/added.dll', 'payload/kmd/bc250kmd.sys', 'install.cmd'))
$plan = @(Get-OrphanFilePlan -PreviousManifest $prevManifest -NewManifest $newManifest -InstallRoot $bdRoot -SystemDir $bdSys -SysWowDir $bdWow)
$want = @((Join-Path $bdRoot 'desktop\old_helper.dll'), (Join-Path $bdRoot 'tools\moved.exe'))
Check (((@($plan | ForEach-Object { $_.path } | Sort-Object) -join ' | ') -eq ((@($want | Sort-Object) -join ' | '))) -and -not @($plan | Where-Object { $_.from_version -ne '0.7.212.100-tester.14' }).Count) "an upgrade removes what the installed release installed and this one does not, the moved file at its old place included, and names the release that installed it: $(@($plan | ForEach-Object { $_.path }) -join ', ')"
Check (-not @(Get-OrphanFilePlan -PreviousManifest $newManifest -NewManifest $newManifest -InstallRoot $bdRoot -SystemDir $bdSys -SysWowDir $bdWow).Count) 'a reinstall or repair of the same version removes nothing'
$stubPrev = New-Bd089Manifest -Version '0.7.212.100-tester.14' -Paths @('payload/system32/bc250umd.dll', 'payload/syswow64/bc250umd.dll')
$stubNew = New-Bd089Manifest -Version '0.7.213.101-tester.16' -Paths @('payload/system32/bc250umd.dll')
$stubPlan = @(Get-OrphanFilePlan -PreviousManifest $stubPrev -NewManifest $stubNew -InstallRoot $bdRoot -SystemDir $bdSys -SysWowDir $bdWow)
Check ((@($stubPlan).Count -eq 1) -and ($stubPlan[0].path -eq (Join-Path $bdWow 'bc250umd.dll'))) "a stub that this release no longer ships is a row for its own folder: $(@($stubPlan | ForEach-Object { $_.path }) -join ', ')"

$keptText = 'a tester put their own file here'
$sameFile = Join-Path $bdRoot 'desktop\old_helper.dll'
$changedFile = Join-Path $bdRoot 'tools\old_tool.exe'
Write-Text $sameFile 'the helper of the older release'
Write-Text $changedFile $keptText
$hashes = @{ 'payload/desktop/old_helper.dll' = (Get-TextSha 'the helper of the older release'); 'payload/tools/old_tool.exe' = (Get-TextSha 'what the older release installed') }
$resolvePrev = New-Bd089Manifest -Version '0.7.212.100-tester.14' -Paths @('payload/desktop/old_helper.dll', 'payload/tools/old_tool.exe', 'payload/tools/gone.dll') -Hashes $hashes
$resolveNew = New-Bd089Manifest -Version '0.7.213.101-tester.16' -Paths @('payload/d3d12/added.dll')
$rows = @(Resolve-OrphanFilePlan (Get-OrphanFilePlan -PreviousManifest $resolvePrev -NewManifest $resolveNew -InstallRoot $bdRoot -SystemDir $bdSys -SysWowDir $bdWow))
$states = @{}; foreach ($row in $rows) { $states[$row.path] = $row.state }
Check ((@($rows).Count -eq 3) -and ($states[$sameFile] -eq 'remove') -and ($states[$changedFile] -eq 'changed') -and ($states[(Join-Path $bdRoot 'tools\gone.dll')] -eq 'absent')) "the file with the recorded SHA256 is removed, the one with other bytes is kept and reported, the one that is gone is absent: $(@($rows | ForEach-Object { "$(Split-Path $_.path -Leaf)=$($_.state)" }) -join ', ')"
Check (([IO.File]::ReadAllText($changedFile) -eq $keptText) -and (Test-Path -LiteralPath $sameFile)) 'the plan reads only: no file is removed while it is computed'

$inventory = @(
    [pscustomobject]@{ Driver = 'oem9.inf'; OriginalFileName = 'bc250kmd.inf'; Version = '0.7.198.100' }
    [pscustomobject]@{ Driver = 'oem12.inf'; OriginalFileName = 'bc250kmd.inf'; Version = '0.7.208.100' }
    [pscustomobject]@{ Driver = 'oem13.inf'; OriginalFileName = 'nvidia.inf'; Version = '32.0.15.6094' }
    [pscustomobject]@{ Driver = 'oem15.inf'; OriginalFileName = 'bc250kmd.inf'; Version = '0.7.213.101' })
$rowsPnp = @(Get-OurDriverPackageRows -Packages $inventory)
Check (((@($rowsPnp | ForEach-Object { "$($_.published)=$($_.version)" }) -join ', ') -eq 'oem9.inf=0.7.198.100, oem12.inf=0.7.208.100, oem15.inf=0.7.213.101')) 'structured inventory: only our packages, each with its version'
$store = @(
    [pscustomobject]@{ published = 'oem9.inf'; version = '0.7.198.100' }
    [pscustomobject]@{ published = 'oem12.inf'; version = '0.7.208.100' }
    [pscustomobject]@{ published = 'oem15.inf'; version = '0.7.213.101' })
$sp = Get-DriverStoreRemovePlan -Packages $store -BoundPublished 'oem15.inf' -PreviousVersion '0.7.208.100'
Check (((@($sp.remove) -join ',') -eq 'oem9.inf') -and ((@($sp.keep) -join ',') -eq 'oem15.inf,oem12.inf') -and ($sp.previous -eq 'oem12.inf')) "the bound package and the one of the previous repair set stay, the older one goes (remove: $(@($sp.remove) -join ', '))"
$sp = Get-DriverStoreRemovePlan -Packages $store -BoundPublished 'oem15.inf' -PreviousVersion $null
Check (((@($sp.remove) -join ',') -eq 'oem9.inf') -and ($sp.previous -eq 'oem12.inf') -and ($sp.why -match 'no previous repair set')) "no record of a previous repair set: the newest of the others is kept ($($sp.why))"
$sp = Get-DriverStoreRemovePlan -Packages $store -BoundPublished 'oem15.inf' -PreviousVersion '0.7.200.100'
Check (((@($sp.remove) -join ',') -eq 'oem9.inf') -and ($sp.previous -eq 'oem12.inf') -and ($sp.why -match 'has no package in the store')) "a previous repair set without a package in the store: the newest of the others is kept ($($sp.why))"
$sp = Get-DriverStoreRemovePlan -Packages $store -BoundPublished $null -PreviousVersion '0.7.208.100'
Check ((-not @($sp.remove).Count) -and (@($sp.keep).Count -eq 3) -and ($sp.why -match 'cannot be read')) "the package of the GPU unknown: nothing is removed ($($sp.why))"
$sp = Get-DriverStoreRemovePlan -Packages $store -BoundPublished 'oem3.inf' -PreviousVersion '0.7.208.100'
Check ((-not @($sp.remove).Count) -and ($sp.why -match 'is not one of ours')) "the GPU on a package that is not ours: nothing is removed ($($sp.why))"
$sp = Get-DriverStoreRemovePlan -Packages ($store + @([pscustomobject]@{ published = 'oem20.inf'; version = $null })) -BoundPublished 'oem15.inf' -PreviousVersion '0.7.208.100'
Check ((-not @($sp.remove).Count) -and ($sp.why -match 'oem20\.inf')) "a package of ours without a version this reading understands: nothing is removed ($($sp.why))"
$sp = Get-DriverStoreRemovePlan -Packages @($store[2]) -BoundPublished 'oem15.inf' -PreviousVersion '0.7.208.100'
Check ((-not @($sp.remove).Count) -and ((@($sp.keep) -join ',') -eq 'oem15.inf')) 'a first install leaves one package and removes nothing'

'[BD-089] what is left of this release (the list uninstall.ps1 prints at the end)'
# Inventory is mocked: this test must not query DISM or depend on host elevation/language.
$savedInventoryReader = ${function:Read-DriverStorePackages}
function Read-DriverStorePackages { return @() }
$saveStateDir = $script:StateDir
$script:StateDir = Join-Path $work 'footprint-state\installer'
$fpRoot = Join-Path $work 'footprint\install-root'
$saveProfiles = $script:ProfilePathsOverride
$script:ProfilePathsOverride = @((Join-Path $work 'footprint\profiles\a'), (Join-Path $work 'footprint\profiles\b'))
$fp = @(Get-ReleaseFootprint -InstallRoot $fpRoot -State ([pscustomobject]@{ firmware_dir_existed = $true }) -MftKeys @('HKLM:\SOFTWARE\Classes\CLSID\{test}'))
$items = @($fp | ForEach-Object { $_.item })
$wantItems = @('install root', 'System32 stub', 'SysWOW64 stub', 'driver store', 'driver service', 'DP audio interrupt', 'policy keys', 'Vulkan registration', 'H.264 encoder keys', 'scheduled task', 'RunOnce entry', 'shortcuts', 'installer state', 'per-user data', 'per-user tuning recovery', 'certificates', 'GPU firmware', 'control application data', 'system-tuning', 'system-tuning-recovery')
Check ((($items -join ' | ') -eq ($wantItems -join ' | '))) "every item of the release is checked: $($items -join ', ')"
Check (-not @($fp | Where-Object { $_.detail -match '^not read' }).Count) "every probe read its item$(if (@($fp | Where-Object { $_.detail -match '^not read' }).Count) { ': ' + (@($fp | Where-Object { $_.detail -match '^not read' } | ForEach-Object { "$($_.item) $($_.detail)" }) -join '; ') })"
$byItem = @{}; foreach ($row in $fp) { $byItem[$row.item] = $row }
Check ((-not $byItem['install root'].present) -and ($byItem['install root'].detail -match 'is gone')) 'an install root that is not there is gone'
Check ((@($byItem['H.264 encoder keys']).present) -and ($byItem['H.264 encoder keys'].detail -match '\{test\}')) 'the encoder keys come from the caller (uninstall.ps1 reads them with the classes key it uses)'
Check (($byItem['GPU firmware'].kept) -and ($byItem['control application data'].kept) -and ($byItem['per-user tuning recovery'].kept) -and ($byItem['system-tuning'].kept) -and ($byItem['system-tuning-recovery'].kept) -and -not @($fp | Where-Object { $_.kept -and $_.item -notin @('GPU firmware', 'control application data','per-user tuning recovery','system-tuning','system-tuning-recovery') }).Count) 'only pre-existing firmware, control backups and tuning recovery are kept on purpose'
Write-Text (Join-Path $fpRoot 'manifest.json') '{}'
Write-Text (Join-Path $script:StateDir 'state.json') '{}'
$fp2 = @(Get-ReleaseFootprint -InstallRoot $fpRoot -State $null)
$byItem2 = @{}; foreach ($row in $fp2) { $byItem2[$row.item] = $row }
Check ($byItem2['install root'].present -and ($byItem2['install root'].detail -match 'is there')) 'an install root that is there is reported as there'
Check ($byItem2['installer state'].present -and -not $byItem2['installer state'].kept) 'the installer state folder is reported, and it is not a kept item'
Check (-not $byItem2['GPU firmware'].kept) 'a firmware folder that the install created itself is not a kept item'
Check (-not @($fp2 | Where-Object { $_.item -eq 'H.264 encoder keys' } | Where-Object { $_.present }).Count) 'no encoder key given: the row says none'
Check ((-not $byItem['per-user data'].present) -and ($byItem['per-user data'].detail -match 'no per-user release data')) 'no profile with our folder: the per-user row is gone'
$udB = Join-Path $work 'footprint\profiles\b\AppData\Local\amdgpu-wddm'
Write-Text (Join-Path $udB 'vkd3d\x.cache') 'x'
$ud = @(Get-OurUserDataDirs)
Check (($ud.Count -eq 1) -and ($ud[0] -eq $udB)) "one profile with our folder: exactly that folder is found ($($ud -join ', '))"
$fp3 = @(Get-ReleaseFootprint -InstallRoot $fpRoot -State $null)
Check (@($fp3 | Where-Object { $_.item -eq 'per-user data' -and $_.present -and -not $_.kept }).Count -eq 1) 'per-user data that is there is reported as LEFT, not kept'
Remove-PathOrSchedule $udB
Check ((-not (Test-Path -LiteralPath $udB)) -and (-not @(Get-OurUserDataDirs).Count)) 'the uninstaller removal takes the per-user folder away'
${function:Read-DriverStorePackages} = $savedInventoryReader
$script:ProfilePathsOverride = $saveProfiles
$script:StateDir = $saveStateDir

$saveStateDir = $script:StateDir
$script:StateDir = Join-Path $work 'bd089-state'
Write-Text (Join-Path (Get-ClosureDir '0.7.208.100-tester.13') 'manifest.json') '{ "version": "0.7.208.100-tester.13", "kmd_version": "0.7.208.100" }'
Write-Text (Join-Path (Get-ClosureDir 'damaged') 'manifest.json') 'not a manifest'
Check (((Get-RepairSetDriverVersion '0.7.208.100-tester.13') -eq '0.7.208.100') -and ($null -eq (Get-RepairSetDriverVersion 'damaged')) -and ($null -eq (Get-RepairSetDriverVersion '0.7.199.100-tester.11')) -and ($null -eq (Get-RepairSetDriverVersion ''))) 'the driver version of a kept repair set comes from its own manifest; a damaged or missing set has none'
$script:StateDir = $saveStateDir

'[G-EVT] settings-impact rows from the registry-default plans'
$plan = Get-RegistryDefaultPlan -Defaults ([pscustomobject]@{ DpmMode = 1; DpmMaxMHz = 2000; EnableFullWddm = 2 }) -Previous ([pscustomobject]@{ DpmMaxMHz = 1500; EnableFullWddm = 2 }) -Current @{ DpmMaxMHz = 1500; EnableFullWddm = 1 } -Explicit @{} -Owned ([ordered]@{ UnconfirmedStarts = 0 })
$imp = Get-SettingsImpact @(@{ group = 'parameters'; plan = $plan })
$byName = @{}; foreach ($row in $imp.rows) { $byName[$row.name] = $row.decision }
Check (($byName.DpmMode -eq 'set') -and ($byName.DpmMaxMHz -eq 'update') -and ($byName.EnableFullWddm -eq 'kept') -and -not $byName.ContainsKey('UnconfirmedStarts')) "rows: $(($imp.rows | ForEach-Object { "$($_.name)=$($_.decision)" }) -join ', ') (installer-owned values left out)"
Check (($imp.summary.added -eq 1) -and ($imp.summary.updated -eq 1) -and ($imp.summary.kept -eq 1)) 'summary counts'

'[G-VER writer] running-release witness (docs/gui/interfaces.md section 1): loaded image, reply and boot bound; the reader''s rule'
$boot = [ordered]@{ boot_id = 41; boot_utc = '2026-10-04T08:00:00.0000000Z' }
$sysSha = [string]$m.components[0].sha256
$msha = 'AB' * 32
$reading = [ordered]@{ image = [ordered]@{ module = '\SystemRoot\System32\DriverStore\FileRepository\bc250kmd.inf_amd64_x\bc250kmd.sys'; path = 'C:\Windows\System32\DriverStore\FileRepository\bc250kmd.inf_amd64_x\bc250kmd.sys'; sha256 = $sysSha
        created_utc = '2026-10-03T20:00:00.0000000Z'; written_utc = '2026-10-03T19:00:00.0000000Z' }; reply_version = '0x000700C7' }
$w = Get-RunningReleaseWitness -Manifest $m -ManifestSha256 $msha -Reading $reading -Boot $boot -RecordedBy 'verify' -Utc '2026-10-04T08:01:00.0000000Z'
$json = $w.record | ConvertTo-Json -Depth 4
$back = $json | ConvertFrom-Json
$keys = @('schema', 'boot_id', 'recorded_utc', 'recorded_by', 'release', 'version', 'manifest_sha256', 'kmd_image_sha256', 'kmd_build', 'kmd_abi')
Check ((@($keys | Where-Object { $null -eq $back.$_ -or [string]$back.$_ -eq '' }).Count -eq 0) -and ($back.schema -eq 1) -and ($json -match '"schema":\s*1,') -and ($json -match '"boot_id":\s*41,') -and ($back.recorded_by -eq 'verify') -and ($back.version -eq $m.version) -and ($back.release -eq $m.name) -and ($back.kmd_image_sha256 -eq $sysSha) -and ($back.kmd_abi -eq '0x000700C7') -and ($back.manifest_sha256 -eq $msha) -and ($back.kmd_build -match '^\d+\.\d+\.\d+\.\d+$')) 'matching image and reply: a witness with every field of section 1, none empty (schema and boot_id as numbers, kmd_build four parts)'
foreach ($c in @(
        @{ name = 'another image'; edit = { param($r) $r.image.sha256 = ('F' * 64) }; want = 'has SHA256' }
        @{ name = 'another reply'; edit = { param($r) $r.reply_version = '0x000700C6' }; want = 'replies 0x000700C6' }
        @{ name = 'no reply'; edit = { param($r) $r.reply_version = $null }; want = 'no driver reply' }
        @{ name = 'not loaded'; edit = { param($r) $r.image = $null }; want = 'no bc250kmd.sys among the loaded drivers' }
        @{ name = 'image file written after the boot started'; edit = { param($r) $r.image.written_utc = '2026-10-04T08:00:30.0000000Z' }; want = 'replaced after this boot started' }
        @{ name = 'image file created after the boot started'; edit = { param($r) $r.image.created_utc = '2026-10-04T08:00:30.0000000Z' }; want = 'replaced after this boot started' }
        @{ name = 'image times unreadable'; edit = { param($r) $r.image.created_utc = $null }; want = 'times of the loaded image' })) {
    $rd = [ordered]@{ image = $(if ($reading.image) { [ordered]@{ module = $reading.image.module; path = $reading.image.path; sha256 = $reading.image.sha256; created_utc = $reading.image.created_utc; written_utc = $reading.image.written_utc } }); reply_version = $reading.reply_version }
    & $c.edit $rd
    $w2 = Get-RunningReleaseWitness -Manifest $m -ManifestSha256 $msha -Reading $rd -Boot $boot -RecordedBy 'verify'
    Check ((-not $w2.record) -and ($w2.reason -match [regex]::Escape($c.want))) "$($c.name): no witness ($($w2.reason))"
}
$w3 = Get-RunningReleaseWitness -Manifest $m -ManifestSha256 $msha -Reading $reading -Boot ([ordered]@{ boot_id = $null; boot_utc = $null }) -RecordedBy 'verify'
Check (-not $w3.record) 'unknown boot: no witness'
$w4 = Get-RunningReleaseWitness -Manifest $m -ManifestSha256 $msha -Reading $reading -Boot $boot -RecordedBy 'start-confirm' -State ([pscustomobject]@{ mutation_boot_id = 41; mutation_utc = '2026-10-04T08:00:40.0000000Z' })
Check ((-not $w4.record) -and ($w4.reason -match 'install action ran in this boot')) "an install action in this boot: no witness until the next start ($($w4.reason))"
$w5 = Get-RunningReleaseWitness -Manifest $m -ManifestSha256 $msha -Reading $reading -Boot $boot -RecordedBy 'start-confirm' -State ([pscustomobject]@{ mutation_boot_id = 40; mutation_utc = '2026-10-03T21:00:00.0000000Z' })
Check ([bool]$w5.record) 'an install action of an earlier boot: the witness is written'
$w6 = Get-RunningReleaseWitness -Manifest $m -ManifestSha256 'ABC' -Reading $reading -Boot $boot -RecordedBy 'verify'
Check ((-not $w6.record) -and ($w6.reason -match 'no SHA256')) 'a manifest hash that is not 64 hex digits: no witness'
foreach ($c in @(@{ f = 'kmd_build'; v = '0.7.199'; want = 'four-part' }, @{ f = 'kmd_abi'; v = '7'; want = '8 hex digits' }, @{ f = 'name'; v = ''; want = 'no release' })) {
    $m2 = $m | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $m2.($c.f) = $c.v
    $w7 = Get-RunningReleaseWitness -Manifest $m2 -ManifestSha256 $msha -Reading $reading -Boot $boot -RecordedBy 'verify'
    Check ((-not $w7.record) -and ($w7.reason -match $c.want)) "manifest $($c.f) '$($c.v)': no witness ($($w7.reason))"
}
$opath = Join-Path $work 'owner-test.json'
[IO.File]::WriteAllText($opath, '{}')
$why = Set-AdminOwner $opath
$own = (Get-Acl -LiteralPath $opath).GetOwner([Security.Principal.SecurityIdentifier]).Value
Check ((-not $why -and $own -in 'S-1-5-32-544', 'S-1-5-18') -or ($why -and -not (([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)))) "owner of a written record: Administrators or SYSTEM ($(if ($own -in 'S-1-5-32-544', 'S-1-5-18') { $own } else { 'not elevated here: the owner stays this user, and the writer reports it' }))"
Remove-Item -LiteralPath $opath -Force -ErrorAction SilentlyContinue
$wit = $back
Check ((Test-RunningReleaseWitness -Witness $wit -Boot $boot -Reply '0x000700C7' -State $null).valid) 'reader: witness of this boot, no install action: valid'
Check (-not (Test-RunningReleaseWitness -Witness $wit -Boot ([ordered]@{ boot_id = 42 }) -Reply '0x000700C7').valid) 'reader: witness of an earlier boot: ignored'
Check (-not (Test-RunningReleaseWitness -Witness $wit -Boot $boot -Reply '0x000700C8').valid) 'reader: another driver reply: not this release'
Check (-not (Test-RunningReleaseWitness -Witness $wit -Boot $boot -Reply '0x000700C7' -State ([pscustomobject]@{ mutation_boot_id = 41; mutation_utc = '2026-10-04T08:05:00.0000000Z' })).valid) 'reader: an install action after the witness in this boot voids it'
Check ((Test-RunningReleaseWitness -Witness $wit -Boot $boot -Reply '0x000700C7' -State ([pscustomobject]@{ mutation_boot_id = 41; mutation_utc = '2026-10-04T07:59:00.0000000Z' })).valid) 'reader: an install action before the witness leaves it valid'
Check ((Test-RunningReleaseWitness -Witness $wit -Boot $boot -Reply '0x000700C7' -State ([pscustomobject]@{ mutation_boot_id = 40; mutation_utc = '2026-10-04T09:00:00.0000000Z' })).valid) 'reader: an install action of another boot leaves it valid'
$wpath = Join-Path $work 'running-release.json'
$testStatePath = Join-Path $work 'absent-state\state.json'
$why = Write-RunningReleaseWitness -InstallRoot $pkg -RecordedBy 'verify' -Boot $boot -Path $wpath -StatePath $testStatePath
Check (($why -match 'no bc250kmd\.sys among the loaded drivers|no driver reply') -and -not (Test-Path -LiteralPath $wpath)) "on this PC (no driver): nothing written ($why)"
$why = Write-RunningReleaseWitness -InstallRoot (Join-Path $work 'nowhere') -RecordedBy 'start-confirm' -Path $wpath -StatePath $testStatePath
Check (($why -match 'no manifest\.json') -and -not (Test-Path -LiteralPath $wpath)) 'no installed release: nothing written, no exception'
Check ($script:WitnessPath -eq (Join-Path $env:ProgramData 'amdgpu-wddm\installer\running-release.json')) "the witness lives in the installer's state folder: $($script:WitnessPath)"

'[G-VER writer] publication under the installer lock: no install action between the readings and the witness (R4)'
# The writer reaches publication when it returns nothing, or (not elevated here) removes the file again because its
# owner cannot be Administrators: both mean that it published.
$wsDir = Join-Path $work 'witness-state'
$wsPath = Join-Path $wsDir 'state.json'
Write-Text $wsPath '{ "schema": 1, "phase": "verified" }'
function Test-Published([string]$Why) { return ((-not $Why) -or ($Why -match 'witness removed again')) }
$wp = Join-Path $work 'r4\running-release.json'
$why = Write-RunningReleaseWitness -InstallRoot $pkg -RecordedBy 'start-confirm' -Boot $boot -Path $wp -StatePath $wsPath -Reading $reading
Check (Test-Published $why) "lock free: the start-confirm writer takes engine.lock and publishes ($(if ($why) { $why } else { 'written' }))"
$held = Open-InstallerLock -Directory $wsDir -WaitMs 0
$clock = [Diagnostics.Stopwatch]::StartNew()
$why = Write-RunningReleaseWitness -InstallRoot $pkg -RecordedBy 'start-confirm' -Boot $boot -Path (Join-Path $work 'r4\busy.json') -StatePath $wsPath -Reading $reading -LockWaitMs 600
Check (($why -match 'holds engine\.lock') -and -not (Test-Path -LiteralPath (Join-Path $work 'r4\busy.json')) -and $clock.ElapsedMilliseconds -lt 5000) "an installer holds engine.lock: no witness after a bounded wait of $($clock.ElapsedMilliseconds) ms ($why)"
$why = Write-RunningReleaseWitness -InstallRoot $pkg -RecordedBy 'verify' -Boot $boot -Path (Join-Path $work 'r4\held.json') -StatePath $wsPath -Reading $reading -LockHeld
Check (Test-Published $why) 'verify, which holds engine.lock itself (-LockHeld): no second acquisition, publishes'
$held.Dispose()
$script:blocked = $null
$why = Write-RunningReleaseWitness -InstallRoot $pkg -RecordedBy 'start-confirm' -Boot $boot -Path (Join-Path $work 'r4\race.json') -StatePath $wsPath -Reading $reading -BeforePublish { $script:blocked = (Open-InstallerLock -Directory $wsDir -WaitMs 0) }
Check (($null -eq $script:blocked) -and (Test-Published $why)) 'an installer that starts between the readings and the publication cannot take engine.lock'
if ($script:blocked) { $script:blocked.Dispose() }
$why = Write-RunningReleaseWitness -InstallRoot $pkg -RecordedBy 'start-confirm' -Boot $boot -Path (Join-Path $work 'r4\mutated.json') -StatePath $wsPath -Reading $reading -BeforePublish {
    Write-Text $wsPath (([ordered]@{ schema = 1; phase = 'files-copied'; mutation_boot_id = 41; mutation_utc = '2026-10-04T08:02:00.0000000Z' }) | ConvertTo-Json) }
Check (($why -match 'recorded during the reading') -and -not (Test-Path -LiteralPath (Join-Path $work 'r4\mutated.json'))) "an install action of this boot recorded between the readings and the publication: no witness ($why)"

'[engine] install action record in state.json (docs/gui/interfaces.md section 2): dry runs record nothing'
$script:state = [pscustomobject]@{ schema = 1; phase = 'new' }
$script:DryRunMode = $true
Set-MutationRecord -Save
Check ((-not $script:state.PSObject.Properties['mutation_utc']) -and -not (Test-Path -LiteralPath $script:StatePath)) 'dry run: no record'
$script:DryRunMode = $false
Set-MutationRecord -Save
$saved = Get-Content -LiteralPath $script:StatePath -Raw | ConvertFrom-Json
$t0 = [string]$saved.mutation_utc
Check (($null -ne $saved.mutation_boot_id) -and ($saved.mutation_boot_id -eq (Get-BootIdentity).boot_id) -and $t0) "first change: saved at once, boot $($saved.mutation_boot_id), $t0"
Start-Sleep -Milliseconds 20
Set-MutationRecord
Check (([string]$script:state.mutation_utc -gt $t0) -and ([string](Get-Content -LiteralPath $script:StatePath -Raw | ConvertFrom-Json).mutation_utc -eq $t0)) 'a later change moves the time on in memory; the next save of the state writes it'
# R3: the first change of a run cannot be made when its record cannot be saved (state.json not writable).
$script:state = [pscustomobject]@{ schema = 1; phase = 'testsigning-active' }
$script:Mutated = $false; $script:EngineMutationSeen = $false
$script:OnFirstChange = { Set-MutationRecord -Save }; $script:OnMutation = { Set-MutationRecord }
$realSave = ${function:Save-InstallState}
Set-Item -Path function:Save-InstallState -Value { param($State) throw 'test: state.json is read-only' }
$script:actionRan = $false; $err = $null
try { [void](Invoke-Change 'a test change' { $script:actionRan = $true }) } catch { $err = $_.Exception.Message }
Set-Item -Path function:Save-InstallState -Value $realSave
Check ((-not $script:actionRan) -and ($err -match 'could not be recorded') -and (-not $script:Mutated) -and (-not $script:EngineMutationSeen)) "the record cannot be saved: the change does not run, the run claims no change ($err)"
$script:actionRan = $false
[void](Invoke-Change 'a test change' { $script:actionRan = $true })
Check ($script:actionRan -and $script:Mutated -and (Get-Content -LiteralPath $script:StatePath -Raw | ConvertFrom-Json).mutation_boot_id -eq (Get-BootIdentity).boot_id) 'with the record saved, the same change runs'
$script:Mutated = $false; $script:EngineMutationSeen = $false; $script:OnFirstChange = $null; $script:OnMutation = $null
$script:state = $null

'[engine] one mutating engine at a time'
$lockDir = Join-Path $work 'lock'
Check (Enter-EngineLock $lockDir) 'first engine takes the lock'
$first = $script:EngineLock
Check (-not (Enter-EngineLock $lockDir)) 'a second engine is refused while the first runs'
$first.Dispose()
Check (Enter-EngineLock $lockDir) 'the lock is free again when the first engine ends'
$script:EngineLock.Dispose()
$script:DryRunMode = $true
Check (Enter-EngineLock $lockDir) 'a dry run takes no lock'
$script:DryRunMode = $false

'[C7] compatibility record'
$cv = Test-CompatibilityRecord -PackageRoot $pkg
Check ($cv.ok -and ($cv.record.no_live_rebind.reboot_directive) -and ((@($cv.record.no_live_rebind.install_sections) -join ',') -eq 'Bc250_Install')) 'a compliant package verifies'
foreach ($c in @(
        @{ name = 'tester.10 shape (INF without the Reboot directive)'; args = @{ Inf = $infTester10 }; want = 'compat.live-rebind' }
        @{ name = 'no record'; args = @{ NoRecord = $true }; want = 'compat.missing' }
        @{ name = 'unknown record schema'; args = @{ EditRecord = { param($r) $r.schema = 'amdgpu-wddm.compatibility/0' } }; want = 'compat.unknown-schema' }
        @{ name = 'another firmware set'; args = @{ EditRecord = { param($r) $r.firmware.files[0].sha256 = ('0' * 64) } }; want = 'compat.firmware' }
        @{ name = 'another engine contract'; args = @{ EditRecord = { param($r) $r.engine.contract = 'amdgpu-wddm.engine/0' } }; want = 'compat.engine' }
        @{ name = 'record claims the directive, INF lacks it'; args = @{ Inf = $infTester10; EditRecord = { param($r) $r.no_live_rebind.reboot_directive = $true } }; want = 'compat.live-rebind' })) {
    $d = Join-Path $work ('compat-' + ($c.name -replace '[^a-z0-9]+', '-'))
    $a = $c.args
    [void](New-FakePackage -Dir $d @a)
    $cv = Test-CompatibilityRecord -PackageRoot $d
    Check ((-not $cv.ok) -and (@($cv.reasons) -contains $c.want)) "$($c.name): refused ($(@($cv.reasons) -join ', '))"
}
$cv = Test-CompatibilityRecord -PackageRoot $pkg -FirmwareDir $badFw
Check ((-not $cv.ok) -and (@($cv.reasons) -contains 'compat.firmware-incomplete')) 'a set whose firmware folder is incomplete is refused'
$cv = Test-CompatibilityRecord -PackageRoot $pkg -FirmwareDir $fwDir
Check $cv.ok 'a set with its complete firmware verifies'
[IO.File]::AppendAllText((Join-Path $pkg 'compatibility.json'), ' ')
$cv = Test-CompatibilityRecord -PackageRoot $pkg
Check ((-not $cv.ok) -and (@($cv.reasons) -contains 'compat.package-damaged')) 'a record changed after the build fails the package integrity'
# R11: a listed, hash-valid record that is not one JSON object of the schema is no record.
foreach ($c in @(@{ name = 'JSON null'; text = 'null' }, @{ name = 'a number'; text = '7' }, @{ name = 'a string'; text = '"amdgpu-wddm.compatibility/1"' },
        @{ name = 'an array'; text = '[ { "schema": "amdgpu-wddm.compatibility/1" } ]' }, @{ name = 'an empty file'; text = '' }, @{ name = 'an empty object'; text = '{}' })) {
    $d = Join-Path $work ('compat-raw-' + ($c.name -replace '[^a-z0-9]+', '-'))
    [void](New-FakePackage -Dir $d -RawRecord -RecordText $c.text)
    $cv = Test-CompatibilityRecord -PackageRoot $d
    Check ((-not $cv.ok) -and ($null -eq $cv.record) -and (@($cv.reasons | Where-Object { $_ -in 'compat.unknown-schema', 'compat.unreadable' }).Count -ge 1)) "$($c.name) as the record: refused ($(@($cv.reasons) -join ', '))"
}

'[G-STAGE] pending restarts: a phase advances only with positive evidence of a new boot (R1, Get-PendingRestart)'
foreach ($ph in 'testsigning-pending', 'driver-pending-restart', 'installed') {
    $st = [pscustomobject]@{ schema = 1; phase = $ph; restart_boot_id = 41 }
    $noSaved = [pscustomobject]@{ schema = 1; phase = $ph }
    Check ((Get-PendingRestart $st ([ordered]@{ boot_id = 41 })) -eq 'same-boot') "${ph}: the same boot keeps it pending"
    Check ((Get-PendingRestart $noSaved ([ordered]@{ boot_id = 42 })) -eq 'saved-unknown') "${ph}: no saved boot keeps it pending"
    Check ((Get-PendingRestart $st ([ordered]@{ boot_id = $null })) -eq 'current-unknown') "${ph}: an unreadable current BootId keeps it pending"
    Check ($null -eq (Get-PendingRestart $st ([ordered]@{ boot_id = 42 }))) "${ph}: another boot lets it advance"
}
Check ($null -eq (Get-PendingRestart ([pscustomobject]@{ phase = 'verified'; restart_boot_id = 41 }) ([ordered]@{ boot_id = 41 }))) 'a phase that waits for nothing is never pending'

'[K3] a native program under a deadline: the budget covers the program, not only the waiting after it'
# Audit finding K3. Restart-GpuAudioDevice promised one budget for pnputil and the status polling, but the
# pnputil call itself was an ordinary blocking pipeline with no timeout, so a stuck PnP restart could hold the
# install for ever. The fake child below never answers inside its budget. It is a child of this test and the
# test stops it afterwards; the installer deliberately does not stop pnputil, because a kernel PnP transition
# cannot be taken back by killing the program that asked for it.
$fakeDir = Join-Path $work 'k3'
[void][IO.Directory]::CreateDirectory($fakeDir)
$fakeSlow = Join-Path $fakeDir 'fake-pnputil.cmd'
Write-Text $fakeSlow "@echo off`r`necho fake pnputil %*`r`nping -n 21 127.0.0.1 >nul`r`n"
$fakeFast = Join-Path $fakeDir 'fake-fast.cmd'
Write-Text $fakeFast "@echo off`r`necho fake fast %*`r`nexit /b 0`r`n"
$fast = Invoke-NativeBounded -File $fakeFast -Arguments @('/restart-device', 'PCI\x') -TimeoutSeconds 30
Check (($fast.code -eq 0) -and (-not $fast.timed_out) -and ($fast.text -match 'fake fast /restart-device')) "a program that answers: exit $($fast.code), timed_out $($fast.timed_out), text '$($fast.text)'"
$clockNew = [Diagnostics.Stopwatch]::StartNew()
$slow = Invoke-NativeBounded -File $fakeSlow -Arguments @('/restart-device', 'PCI\x') -TimeoutSeconds 2
$newSeconds = $clockNew.Elapsed.TotalSeconds
Check ($slow.timed_out -and ($null -eq $slow.code) -and ($slow.id -gt 0)) "a program that does not answer: timed_out $($slow.timed_out), code '$($slow.code)', process $($slow.id)"
Check ($newSeconds -lt 10) "the deadline of 2 s returned after $([Math]::Round($newSeconds, 1)) s"
Check ($slow.text -match 'fake pnputil /restart-device') "the output up to the deadline comes back ('$($slow.text)')"
Stop-Process -Id $slow.id -Force -ErrorAction SilentlyContinue
# The whole function, with the fake in place of pnputil and no real device to ask about: the timeout is its own
# answer, ok is false, and the saved MSI value is nobody's business here (the caller writes it before this call).
$savedPnpUtil = $script:PnpUtilPath
function Get-PnpDevice { param([string]$InstanceId, [switch]$ErrorAction) return $null }
try {
    $script:PnpUtilPath = $fakeSlow
    $clockFn = [Diagnostics.Stopwatch]::StartNew()
    $r3 = Restart-GpuAudioDevice -InstanceId 'PCI\VEN_1002&DEV_13FF&SUBSYS_00000000&REV_00\4&0&1' -TimeoutSeconds 2
    $fnSeconds = $clockFn.Elapsed.TotalSeconds
} finally { $script:PnpUtilPath = $savedPnpUtil }
Check ($r3.timed_out -and (-not $r3.ok) -and ($null -eq $r3.status)) "Restart-GpuAudioDevice on a stuck restart: timed_out $($r3.timed_out), ok $($r3.ok), status '$($r3.status)'"
Check ($fnSeconds -lt 10) "and it returned after $([Math]::Round($fnSeconds, 1)) s, inside its 2 s budget plus the start-up of the fake"
Stop-Process -Id $r3.id -Force -ErrorAction SilentlyContinue
# The negative control of the same section: the unbounded helper the function used before this change takes as
# long as the program does, whatever the caller's budget says.
$fakeSix = Join-Path $fakeDir 'fake-six.cmd'
Write-Text $fakeSix "@echo off`r`nping -n 7 127.0.0.1 >nul`r`n"
$clockOld = [Diagnostics.Stopwatch]::StartNew()
$null = Invoke-Native $fakeSix @('/restart-device', 'PCI\x')
$oldSeconds = $clockOld.Elapsed.TotalSeconds
Check ($oldSeconds -ge 5) "negative control: Invoke-Native (no deadline) took $([Math]::Round($oldSeconds, 1)) s for a 6 s program and observes no budget"

'[BD-125] Secure Boot from the UEFI variable (fallbacks only when the read fails); the certificate step names the store'
$sbCalls = @{ confirm = 0; registry = 0 }
function Read-Sb($Variable, $Confirm = { $true }, $Registry = { $null }) {
    $sbCalls.confirm = 0; $sbCalls.registry = 0
    return Read-SecureBootState -ReadVariable $Variable -ConfirmUefi { $sbCalls.confirm++; & $Confirm }.GetNewClosure() -ReadRegistry { $sbCalls.registry++; & $Registry }.GetNewClosure()
}
$s = Read-Sb { [pscustomobject]@{ bytes = [byte[]]@(1); error = 0; privilege_error = 0 } }
Check (($s.state -eq 'on') -and ($s.source -eq 'uefi-variable') -and ($sbCalls.confirm -eq 0)) "variable 1: $($s.state), $($s.detail)"
$s = Read-Sb { [pscustomobject]@{ bytes = [byte[]]@(0); error = 0; privilege_error = 0 } }
Check (($s.state -eq 'off') -and ($s.source -eq 'uefi-variable') -and ($sbCalls.confirm -eq 0)) "variable 0: $($s.state), $($s.detail)"
# The stock BC-250 firmware (BD-125): no variable. Confirm-SecureBootUEFI would throw and the registry key is absent.
$s = Read-Sb { [pscustomobject]@{ bytes = $null; error = 203; privilege_error = 0 } } { throw 'Variable is currently undefined: 0xC0000100' }
Check (($s.state -eq 'off') -and ($s.detail -like 'off (not built into the firmware*') -and ($s.detail -match 'Win32 error 203') -and ($sbCalls.confirm -eq 0) -and ($sbCalls.registry -eq 0)) "no variable (ERROR_ENVVAR_NOT_FOUND): $($s.state), $($s.detail)"
$s = Read-Sb { [pscustomobject]@{ bytes = $null; error = 1; privilege_error = 0 } }
Check (($s.state -eq 'legacy-bios') -and ($s.source -eq 'uefi-variable') -and ($sbCalls.confirm -eq 0)) "ERROR_INVALID_FUNCTION: $($s.state), $($s.detail)"
$s = Read-Sb { [pscustomobject]@{ bytes = $null; error = 1314; privilege_error = 1300 } } { $false }
Check (($s.state -eq 'off') -and ($s.source -eq 'confirm-securebootuefi') -and ($s.detail -match 'Win32 error 1314') -and ($s.detail -match 'Win32 error 1300')) "read refused, Confirm-SecureBootUEFI decides: $($s.detail)"
$s = Read-Sb { [pscustomobject]@{ bytes = [byte[]]@(2); error = 0; privilege_error = 0 } } { $true }
Check (($s.state -eq 'on') -and ($s.source -eq 'confirm-securebootuefi') -and ($s.detail -match 'unexpected value: 02')) "an unexpected byte goes to the fallback: $($s.detail)"
$s = Read-Sb { [pscustomobject]@{ bytes = $null; error = 5; privilege_error = 0 } } { throw (New-Object PlatformNotSupportedException) }
Check (($s.state -eq 'legacy-bios') -and ($s.source -eq 'confirm-securebootuefi')) "Confirm-SecureBootUEFI on a legacy BIOS: $($s.detail)"
$s = Read-Sb { throw 'no Add-Type' } { throw 'denied' } { 1 }
Check (($s.state -eq 'on') -and ($s.source -eq 'registry') -and ($s.detail -match 'did not run') -and ($sbCalls.registry -eq 1)) "the read could not run, Confirm-SecureBootUEFI failed, the registry decides: $($s.detail)"
# Negative control: the shape the old reader saw on the BC-250 (no variable answer, Confirm throws, no registry value)
# stays unknown and names every source; only the variable read turns it into "off".
$s = Read-Sb { [pscustomobject]@{ bytes = $null; error = 87; privilege_error = 0 } } { throw 'Variable is currently undefined: 0xC0000100' } { $null }
Check (($s.state -eq 'unknown') -and ($s.source -eq 'none') -and ($s.detail -match 'Win32 error 87') -and ($s.detail -match 'Confirm-SecureBootUEFI: ') -and ($s.detail -match 'no SecureBoot\\State')) "no source answers: $($s.state), $($s.detail)"
$live = Read-SecureBootVariable
Check (($null -ne $live.bytes) -or ($live.error -gt 0)) "the real read compiles and runs on this host: bytes $(if ($null -ne $live.bytes) { [BitConverter]::ToString([byte[]]$live.bytes) } else { 'none' }), Win32 error $($live.error), privilege error $($live.privilege_error)"
Check ((Get-SecureBootState) -in 'on', 'off', 'legacy-bios', 'unknown') "Get-SecureBootState keeps its four values on this host: $(Get-SecureBootState)"

$cerPath = 'C:\pkg dir\payload\cert\amdgpu-wddm-release.cer'
$denied = New-Object UnauthorizedAccessException 'Access is denied.'
$e1 = New-CertificateStoreError -Store 'Root' -Certificate $cerPath -Exception $denied -Remaining @('TrustedPublisher')
Check (($e1 -is [Runtime.InteropServices.COMException]) -and ($e1.HResult -eq $denied.HResult) -and ('0x{0:X8}' -f $e1.HResult) -eq '0x80070005') "the error keeps the HRESULT: $('0x{0:X8}' -f $e1.HResult)"
Check (($e1.Message -match [regex]::Escape('LocalMachine\Root, HRESULT 0x80070005 (E_ACCESSDENIED)')) -and ($e1.Message -notmatch 'already has it') -and ($e1.Message -match 'antivirus') -and ($e1.Message -match 'continues from here') -and ($e1.Message.Contains("certutil -addstore -f Root `"$cerPath`" and then certutil -addstore -f TrustedPublisher `"$cerPath`"")) -and ($e1.Message -match 'Windows said: Access is denied\.')) "Root refused: $($e1.Message)"
$e2 = New-CertificateStoreError -Store 'TrustedPublisher' -Certificate $cerPath -Exception $denied -Done @('Root')
Check (($e2.Message -match [regex]::Escape('LocalMachine\TrustedPublisher, HRESULT 0x80070005')) -and ($e2.Message -match [regex]::Escape('LocalMachine\Root already has it')) -and ($e2.Message.Contains("certutil -addstore -f TrustedPublisher `"$cerPath`".")) -and ($e2.Message -notmatch 'addstore -f Root')) "TrustedPublisher refused after Root: $($e2.Message)"
$e3 = New-CertificateStoreError -Store 'Root' -Certificate $cerPath -Exception (New-Object IO.IOException 'busy', -2147024864)
Check (($e3.Message -match 'HRESULT 0x80070020\.') -and ($e3.HResult -eq -2147024864)) "another HRESULT keeps its number without a name: $('0x{0:X8}' -f $e3.HResult)"
# Add-CertificateToStore (BD-125 part two): Import-Certificate, then certutil, and the store decides.
$thumb = 'F' * 40
$cuCalls = New-Object Collections.ArrayList
$refuse = { param($File, $Name) throw (New-Object UnauthorizedAccessException 'Access is denied.') }
$cuOk = { param($File, $Name) [void]$cuCalls.Add("$Name|$File"); @{ text = 'translated text'; code = 0; timed_out = $false } }
$a = Add-CertificateToStore -Store 'Root' -Certificate $cerPath -Thumbprint $thumb -Import { param($File, $Name) } -CertUtil $cuOk -Present { $false }
Check (($a.ok) -and ($a.via -eq 'Import-Certificate') -and ($cuCalls.Count -eq 0) -and ($null -eq $a.exception)) "Import-Certificate adds it: certutil is not run ($($a.detail))"
$a = Add-CertificateToStore -Store 'TrustedPublisher' -Certificate $cerPath -Thumbprint $thumb -Import $refuse -CertUtil $cuOk -Present { param($Name, $T) ($Name -eq 'TrustedPublisher') -and ($T -eq $thumb) }
Check (($a.ok) -and ($a.via -eq 'certutil') -and ($cuCalls.Count -eq 1) -and ($cuCalls[0] -eq "TrustedPublisher|$cerPath") -and ($a.detail -match 'HRESULT 0x80070005') -and ($a.detail -match 'exit code 0') -and ($a.exception -is [UnauthorizedAccessException])) "refused, certutil adds it to the same store: $($a.detail)"
$a = Add-CertificateToStore -Store 'TrustedPublisher' -Certificate $cerPath -Thumbprint $thumb -Import $refuse -CertUtil $cuOk -Present { $false }
Check ((-not $a.ok) -and ($a.via -eq 'none') -and ($a.detail -match 'certutil did not add it \(certutil exit code 0\)')) "certutil exit code 0 but the store lacks the thumbprint: not added ($($a.detail))"
$a = Add-CertificateToStore -Store 'Root' -Certificate $cerPath -Thumbprint $thumb -Import $refuse -CertUtil { throw 'no certutil' } -Present { $false }
Check ((-not $a.ok) -and ($a.detail -match 'certutil did not run: no certutil')) "certutil cannot run: not added ($($a.detail))"
$a = Add-CertificateToStore -Store 'Root' -Certificate $cerPath -Thumbprint $thumb -Import $refuse -CertUtil { @{ text = ''; code = $null; timed_out = $true } } -Present { throw 'store locked' }
Check ((-not $a.ok) -and ($a.detail -match 'did not finish in 60 s') -and ($a.detail -match 'could not be read: store locked')) "certutil times out and the store cannot be read: not added ($($a.detail))"

# The step itself, as install.ps1 ships it, with stand-ins for the three actions of Add-CertificateToStore.
$installText = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
$step = [regex]::Match($installText, '(?s)Invoke-Change "add the release test certificate .*?\n\} \| Out-Null')
Check $step.Success 'install.ps1 has the certificate step'
$realAdd = ${function:Add-CertificateToStore}
$script:held = New-Object Collections.ArrayList
function Run-CertStep([string[]]$ImportRefuses, [string[]]$CertUtilAdds) {
    $held = $script:held   # one list for the closures below: $script: inside a closure is the closure's own module
    $held.Clear()
    $script:stubImport = { param($File, $Name) if ($ImportRefuses -contains $Name) { throw (New-Object UnauthorizedAccessException 'Access is denied.') }; [void]$held.Add($Name) }.GetNewClosure()
    $script:stubCertUtil = { param($File, $Name) if ($CertUtilAdds -contains $Name) { [void]$held.Add($Name) }; @{ text = ''; code = 0; timed_out = $false } }.GetNewClosure()
    $script:stubPresent = { param($Name, $T) $held -contains $Name }.GetNewClosure()
    $warnings = New-Object Collections.ArrayList
    Set-Item Function:\Write-Warn2 { param([string]$Text) [void]$warnings.Add($Text) }.GetNewClosure()
    $cer = $cerPath
    $cert = [pscustomobject]@{ Thumbprint = $thumb }
    $script:DryRunMode = $false; $script:OnFirstChange = $null; $script:OnMutation = $null; $script:OnChange = $null
    $stepError = $null
    try { & ([scriptblock]::Create($step.Value)) 6>$null } catch { $stepError = $_.Exception }
    $script:Mutated = $false
    return [pscustomobject]@{ held = @($held); error = $stepError; warnings = @($warnings) }
}
$realWarn = ${function:Write-Warn2}
Set-Item Function:\Add-CertificateToStore { param($Store, $Certificate, $Thumbprint)
    & $realAdd -Store $Store -Certificate $Certificate -Thumbprint $Thumbprint -Import $script:stubImport -CertUtil $script:stubCertUtil -Present $script:stubPresent }
$r = Run-CertStep -ImportRefuses @('TrustedPublisher') -CertUtilAdds @('TrustedPublisher')
Check (($null -eq $r.error) -and (($r.held -join ',') -eq 'Root,TrustedPublisher') -and ($r.warnings.Count -eq 0)) "TrustedPublisher refuses Import-Certificate, certutil adds it: both stores, no warning ($(($r.held) -join ','))"
$r = Run-CertStep -ImportRefuses @('TrustedPublisher') -CertUtilAdds @()
Check (($null -eq $r.error) -and (($r.held -join ',') -eq 'Root') -and ($r.warnings.Count -eq 1) -and ($r.warnings[0] -match 'LocalMachine\\TrustedPublisher: Import-Certificate was refused \(HRESULT 0x80070005\)') -and ($r.warnings[0] -match 'The install goes on')) "TrustedPublisher refuses both ways after Root: a warning, the install goes on: $($r.warnings -join ' | ')"
$r = Run-CertStep -ImportRefuses @('Root', 'TrustedPublisher') -CertUtilAdds @()
Check (($r.error -is [Runtime.InteropServices.COMException]) -and ($r.held.Count -eq 0) -and ($r.error.Message -match [regex]::Escape('LocalMachine\Root, HRESULT 0x80070005')) -and ($r.error.Message.Contains("certutil -addstore -f Root `"$cerPath`" and then certutil -addstore -f TrustedPublisher `"$cerPath`""))) "Root refuses both ways: the step stops with the hand commands: $(if ($r.error) { $r.error.Message } else { 'no error' })"
$r = Run-CertStep -ImportRefuses @('Root') -CertUtilAdds @('Root')
Check (($null -eq $r.error) -and (($r.held -join ',') -eq 'Root,TrustedPublisher')) "Root refuses Import-Certificate, certutil adds it, TrustedPublisher imports: both stores ($(($r.held) -join ','))"
Set-Item Function:\Add-CertificateToStore $realAdd
Set-Item Function:\Write-Warn2 $realWarn

'[G-STAGE] child closure: the job''s own count decides, an unreadable job is unknown (R9; last: this process joins a job)'
Check (($null -eq [AmdgpuWddmEngine.Job]::Processes()) -and ([AmdgpuWddmEngine.Job]::ActiveProcesses() -eq -1)) 'no job: the listing is unknown (null), never an empty job'
$script:EngineJob = 'kill-on-close'
$c0 = Close-EngineChildren
Check (($c0.closure -eq 'unknown') -and ($null -eq $c0.left_at_exit) -and ($null -eq $c0.remaining)) "a job that cannot be read: closure unknown, no counts ($($c0 | ConvertTo-Json -Compress))"
$why = [AmdgpuWddmEngine.Job]::Enter()
Check ((-not $why) -and ([AmdgpuWddmEngine.Job]::ActiveProcesses() -eq 1) -and (@([AmdgpuWddmEngine.Job]::Processes()) -contains [int64]$PID)) "this process in its kill-on-close job, alone ($why)"
$cmd = Join-Path $env:windir 'System32\cmd.exe'
$k1 = Start-Process -FilePath $cmd -ArgumentList '/d', '/c', 'ping -n 60 127.0.0.1 >nul' -WindowStyle Hidden -PassThru
# A child that starts its grandchild only later, after the first listing of the job.
$k2 = Start-Process -FilePath $cmd -ArgumentList '/d', '/c', 'ping -n 2 127.0.0.1 >nul & ping -n 60 127.0.0.1 >nul' -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 500
$c1 = Close-EngineChildren
Check (($c1.closure -eq 'complete') -and ($c1.remaining -eq 0) -and ($c1.left_at_exit -ge 3) -and ($c1.ended -eq $c1.left_at_exit) -and ([AmdgpuWddmEngine.Job]::ActiveProcesses() -eq 1) -and $k1.HasExited -and $k2.HasExited) "every child and grandchild ended, the job holds only this process ($($c1 | ConvertTo-Json -Compress))"
$c2 = Close-EngineChildren
Check (($c2.closure -eq 'complete') -and ($c2.left_at_exit -eq 0)) 'nothing left: complete with 0'

'[#38] the start state verify reads before it waits and before it probes the adapter'
$ok = Get-VerifyStartState -ProblemCode 0 -Status 'OK'
Check ($ok.started -and ($null -eq $ok.reason)) "problem code 0: the driver started, no reason text ($($ok | ConvertTo-Json -Compress))"
# Code 43 is what the tester's device reported in #38; the others are codes a failed start also shows.
foreach ($code in 43, 10, 28, 31, 39) {
    $bad = Get-VerifyStartState -ProblemCode $code -Status 'Error'
    Check ((-not $bad.started) -and ($bad.reason -match "problem code $code") -and ($bad.reason -match 'Windows reports it as Error')) `
        "problem code ${code}: the driver did not start, and the reason names the code and the status ($($bad.reason))"
}
$noStatus = Get-VerifyStartState -ProblemCode 43
Check ((-not $noStatus.started) -and ($noStatus.reason -match 'problem code 43') -and ($noStatus.reason -notmatch 'Windows reports')) `
    "a device status that is not known: the reason names the code alone ($($noStatus.reason))"
$okButOdd = Get-VerifyStartState -ProblemCode 0 -Status 'Unknown'
Check ($okButOdd.started) 'problem code 0 decides on its own: a status this check does not know is not a failed start'

'[#38] install.ps1 verify: a failed start skips the wait and the probes that need a started adapter'
$verify = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
Check ($verify -match '\$startState = Get-VerifyStartState -ProblemCode \(\[int\]\$dev\[0\]\.ConfigManagerErrorCode\)') 'the start state comes from the device verify already read'
Check ($verify -match '\$noWaitWhy = \$\(if \(\$startState\.started\)') 'the wait reads that one state'
Check ($verify -match 'while \(\$null -eq \$noWaitWhy\)') 'a started driver still waits for the start-confirm task of this logon'
Check ($verify -match 'the wait was skipped because \$noWaitWhy') 'and a verify line says why nothing was waited for'
# Each probe that needs a started adapter: guarded by the same state, recorded under its own name, never a failure.
foreach ($probe in 'D3D12 FL 12_1', 'Vulkan', 'GPU desktop path') {
    Check ($verify -match ("Add-Skipped '" + [regex]::Escape($probe) + "'")) "$probe is recorded as skipped when the driver did not start"
}
Check ($verify -match "function Add-Skipped") 'the skip has one writer'
Check ($verify -match "Add-Info \`$Name \('skipped: the driver did not start'") 'a skipped probe is an info line, so it is not an extra failure'
# The property the line above depends on: an info line is in the result list, so a skipped probe keeps its own name
# in the report the control application reads, with the reason. A reviewer read this the other way round, so it is
# now a check: Add-Info adds to $results before it writes anything to the console.
Check ($verify -match "(?s)function Add-Info\(\[string\]\`$Name, \[string\]\`$Detail\) \{\s*\[void\]\`$results\.Add\(") `
    'an info line, and so a skipped probe, is in the verify report under its own name'
Check (@([regex]::Matches($verify, 'if \(-not \$startState\.started\)')).Count -ge 3) 'three probes ask the state before they run'
Check ($verify -match '\(Test-Path -LiteralPath \$cli\) -and \$startState\.started') 'the driver is not asked for its interop state when it did not start'
Check ($verify -match 'Write-VerifyReport \$results') 'the report is still written on every path'

'the Microsoft Visual C++ runtime: the installer brings it (owner, 2026-10-12)'
# The decision, over readings of the three System32 files. A reading of $null means the file is not there.
function New-VcReading([string]$Version) {
    if (-not $Version) { return $null }
    $p = $Version.Split('.')
    return [pscustomobject]@{ major = [int]$p[0]; minor = [int]$p[1]; build = [int]$p[2]; revision = [int]$p[3] }
}
function New-VcReadings([string]$A, [string]$B, [string]$C) {
    return @{ 'vcruntime140.dll' = (New-VcReading $A); 'vcruntime140_1.dll' = (New-VcReading $B); 'msvcp140.dll' = (New-VcReading $C) }
}
$vcAll = Get-VcRuntimeDecision -Readings (New-VcReadings '14.44.35211.0' '14.44.35211.0' '14.44.35211.0')
Check (-not $vcAll.install -and $vcAll.missing.Count -eq 0 -and $vcAll.old.Count -eq 0 -and $vcAll.present.Count -eq 3) 'the runtime of the package itself needs no install'
$vcNewer = Get-VcRuntimeDecision -Readings (New-VcReadings '14.50.1.0' '14.50.1.0' '14.50.1.0')
Check (-not $vcNewer.install) 'a newer runtime needs no install'
$vcNone = Get-VcRuntimeDecision -Readings (New-VcReadings $null $null $null)
Check ($vcNone.install -and $vcNone.missing.Count -eq 3 -and $vcNone.detail -match 'missing in System32: vcruntime140\.dll, vcruntime140_1\.dll, msvcp140\.dll') 'no runtime at all: install, and the detail names all three files'
$vcOne = Get-VcRuntimeDecision -Readings (New-VcReadings '14.44.35211.0' $null '14.44.35211.0')
Check ($vcOne.install -and $vcOne.missing -contains 'vcruntime140_1.dll' -and $vcOne.present.Count -eq 2) 'one file missing is enough'
$vcOld = Get-VcRuntimeDecision -Readings (New-VcReadings '14.40.33810.0' '14.44.35211.0' '14.44.35211.0')
Check ($vcOld.install -and $vcOld.old.Count -eq 1 -and $vcOld.detail -match 'older than 14\.44: vcruntime140\.dll 14\.40\.33810\.0') 'a 14.40 file is too old, and the detail says so'
$vcOldMajor = Get-VcRuntimeDecision -Readings (New-VcReadings '12.0.0.0' '14.44.35211.0' '14.44.35211.0')
Check ($vcOldMajor.install -and $vcOldMajor.old.Count -eq 1) 'an older major version is too old'
$vcExact = Get-VcRuntimeDecision -Readings (New-VcReadings '14.44.0.0' '14.44.0.0' '14.44.0.0')
Check (-not $vcExact.install) '14.44 itself is enough: the minimum is the major and the minor'
# This host's own reading, through the same decision: it says what it says, and the detail names every file.
$vcHere = Get-VcRuntimeState
Check ($vcHere.detail -and ($vcHere.missing.Count + $vcHere.old.Count + $vcHere.present.Count) -eq 3) "this host's runtime reads as three files ($($vcHere.detail))"

# The file the package carries: SHA-256 first, then a Valid Microsoft signature. The signature reader is handed in,
# so every case is exercised without a signed file of our own.
$vcWork = Join-Path $work 'vcredist'
[void][IO.Directory]::CreateDirectory($vcWork)
$vcFake = Join-Path $vcWork 'vc_redist.x64.exe'
Write-Text $vcFake 'not the real installer'
$vcFakeSha = Get-Sha256 $vcFake
$msCert = [pscustomobject]@{ Subject = 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US' }
$valid = { param($p) [pscustomobject]@{ Status = 'Valid'; SignerCertificate = $msCert } }
$ok = Test-VcRuntimeInstaller -Path $vcFake -ExpectedSha256 $vcFakeSha -SignatureReader $valid
Check ($ok.ok -and $ok.status -eq 'Valid' -and $ok.signer -match 'Microsoft Corporation') 'the right bytes with a valid Microsoft signature are accepted'
$badHash = Test-VcRuntimeInstaller -Path $vcFake -ExpectedSha256 ('0' * 64) -SignatureReader $valid
Check (-not $badHash.ok -and $badHash.problem -match 'SHA256 of .* is ') 'negative control: another SHA-256 is refused, and the signature is not even read'
$badSig = Test-VcRuntimeInstaller -Path $vcFake -ExpectedSha256 $vcFakeSha -SignatureReader { param($p) [pscustomobject]@{ Status = 'HashMismatch'; SignerCertificate = $msCert } }
Check (-not $badSig.ok -and $badSig.problem -match 'signature of .* is not valid \(status HashMismatch\)') 'negative control: a signature that is not Valid is refused'
$unsigned = Test-VcRuntimeInstaller -Path $vcFake -ExpectedSha256 $vcFakeSha -SignatureReader { param($p) [pscustomobject]@{ Status = 'NotSigned'; SignerCertificate = $null } }
Check (-not $unsigned.ok -and $unsigned.problem -match 'status NotSigned') 'negative control: an unsigned file is refused'
$otherSigner = Test-VcRuntimeInstaller -Path $vcFake -ExpectedSha256 $vcFakeSha -SignatureReader { param($p) [pscustomobject]@{ Status = 'Valid'; SignerCertificate = [pscustomobject]@{ Subject = 'CN=Somebody Else, O=Somebody Else' } } }
Check (-not $otherSigner.ok -and $otherSigner.problem -match 'not signed by Microsoft Corporation') 'negative control: a valid signature of another signer is refused'
$nearSigner = Test-VcRuntimeInstaller -Path $vcFake -ExpectedSha256 $vcFakeSha -SignatureReader { param($p) [pscustomobject]@{ Status = 'Valid'; SignerCertificate = [pscustomobject]@{ Subject = 'CN=Microsoft Corporation Of Nowhere, O=Nowhere' } } }
Check (-not $nearSigner.ok) 'negative control: a signer name that only starts with the expected one is refused'
$absent = Test-VcRuntimeInstaller -Path (Join-Path $vcWork 'nothing-here.exe')
Check (-not $absent.ok -and $absent.problem -match 'does not carry') 'a package without the Microsoft installer is refused'
# The real file of this workspace, when it is there: the pinned SHA-256 and Windows' own signature reading.
$vcReal = @((Join-Path (Split-Path (Split-Path $Installer)) $script:VcRuntimePackagePath), 'P:\BC-250\toolchain\vc-runtime\14.44.35211\vc_redist.x64.exe') | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($vcReal) {
    $real = Test-VcRuntimeInstaller -Path $vcReal
    Check ($real.ok) "the Microsoft installer in hand passes the pinned SHA-256 and Windows' signature reading ($vcReal, $($real.status), $($real.signer))"
    Check ((Get-Item -LiteralPath $vcReal).Length -eq $script:VcRuntimeSize) "its size is the pinned $($script:VcRuntimeSize) bytes"
} else { Check $true 'the Microsoft installer is not in this checkout: the pinned-hash case is covered by the package test' }

# What each exit code of Microsoft's installer means.
Check ((Get-VcRuntimeOutcome 0).ok -and -not (Get-VcRuntimeOutcome 0).restart) 'exit 0: installed'
Check ((Get-VcRuntimeOutcome 1638).ok -and -not (Get-VcRuntimeOutcome 1638).restart -and (Get-VcRuntimeOutcome 1638).message -match 'newer') 'exit 1638: a newer runtime is there'
Check ((Get-VcRuntimeOutcome 3010).ok -and (Get-VcRuntimeOutcome 3010).restart) 'exit 3010: installed, restart wanted'
Check ((Get-VcRuntimeOutcome 1641).ok -and (Get-VcRuntimeOutcome 1641).restart) 'exit 1641: installed, restart wanted'
foreach ($code in 1603, 1618, 5, -1073741819) {
    $out = Get-VcRuntimeOutcome $code
    Check (-not $out.ok -and -not $out.restart -and $out.message -match "code $code") "negative control: exit $code is a failure, and the message names the code"
}
# The runner, with the codes handed in: the arguments Microsoft documents, and the outcome of each code.
$seen = $null
$run0 = Invoke-VcRuntimeInstall -Path $vcFake -Runner { param($p, $a) $script:seen = @($a); 0 }
Check ($run0.ok -and $run0.code -eq 0 -and (($seen -join ' ') -eq '/install /quiet /norestart')) "the install runs Microsoft's installer with /install /quiet /norestart"
Check ((Invoke-VcRuntimeInstall -Path $vcFake -Runner { param($p, $a) 1638 }).ok) 'the install accepts 1638'
Check ((Invoke-VcRuntimeInstall -Path $vcFake -Runner { param($p, $a) 3010 }).restart) 'the install folds 3010 into the restart it makes anyway'
$run1603 = Invoke-VcRuntimeInstall -Path $vcFake -Runner { param($p, $a) 1603 }
Check (-not $run1603.ok -and $run1603.message -match 'code 1603') 'negative control: exit 1603 fails the step with a plain message'
# 1638 with a file still missing from System32: the runtime is registered and its files are not, so the same program
# runs once more with /repair before the installation gives up.
$seenRepair = $null
$runRepair = Invoke-VcRuntimeInstall -Path $vcFake -Arguments $script:VcRuntimeRepairArguments -Runner { param($p, $a) $script:seenRepair = @($a); 0 }
Check ($runRepair.ok -and (($seenRepair -join ' ') -eq '/repair /quiet /norestart')) "the repair run uses Microsoft's /repair /quiet /norestart"
Check ((Invoke-VcRuntimeInstall -Path $vcFake -Arguments $script:VcRuntimeRepairArguments -Runner { param($p, $a) 1603 }).ok -eq $false) 'negative control: a repair that fails is a failure too'
Check ($installText -match "(?s)vcRun\.code -eq 1638.{0,600}VcRuntimeRepairArguments") 'phase 2 repairs the registered runtime when 1638 left System32 without it'
Check ($installText -match 'Repair the Microsoft Visual C\+\+ 2015-2022 Redistributable \(x64\) in Windows Settings') 'the message of a runtime that is still not there asks for a repair, not for the install that just refused'
Check ($installText -notmatch "but the runtime is still not in place[^\r\n]*Install the Microsoft Visual C\+\+ 2015-2022 Redistributable \(x64\) yourself") 'the self-contradicting remedy is gone from that message'
# The host-test reading of System32, so a dry run can walk the step on a PC that already has the runtime. It is a
# dry run only: a real install always reads System32.
$script:DryRunMode = $true
try {
    $env:AMDGPU_WDDM_TEST_VCRUNTIME = 'missing'
    $vcFaked = Get-VcRuntimeState
    Check ($vcFaked.install -and $vcFaked.missing.Count -eq 3) 'a dry run can read System32 as a PC without the runtime'
    $env:AMDGPU_WDDM_TEST_VCRUNTIME = 'old'
    $vcFakedOld = Get-VcRuntimeState
    Check ($vcFakedOld.install -and $vcFakedOld.old.Count -eq 3) 'and as a PC whose runtime is older than 14.44'
    $env:AMDGPU_WDDM_TEST_VCRUNTIME = 'ok'
    Check (-not (Get-VcRuntimeState).install) 'and as a PC that has it'
    $script:DryRunMode = $false
    $env:AMDGPU_WDDM_TEST_VCRUNTIME = 'missing'
    $vcReadReal = Get-VcRuntimeState
    Check ($vcReadReal.detail -eq (Get-VcRuntimeDecision -Readings @{
        'vcruntime140.dll' = (Get-FileVersionNumbers (Join-Path $env:windir 'System32\vcruntime140.dll'))
        'vcruntime140_1.dll' = (Get-FileVersionNumbers (Join-Path $env:windir 'System32\vcruntime140_1.dll'))
        'msvcp140.dll' = (Get-FileVersionNumbers (Join-Path $env:windir 'System32\msvcp140.dll')) }).detail) 'negative control: a run that is not a dry run ignores the host-test reading'
} finally { Remove-Item Env:\AMDGPU_WDDM_TEST_VCRUNTIME -ErrorAction SilentlyContinue; $script:DryRunMode = $false }
# One real run through the bounded runner, so the plumbing from exit code to outcome is exercised for real.
$realRun = Invoke-NativeBounded -File $env:ComSpec -Arguments @('/c', 'exit', '1603') -TimeoutSeconds 60
Check (-not $realRun.timed_out -and (Get-VcRuntimeOutcome $realRun.code).message -match 'code 1603') 'a program that ends with 1603 reaches the same outcome through Invoke-NativeBounded'

# The engine's own side: the preflight no longer refuses a PC without the runtime, and phase 2 installs it before
# any file of ours is copied.
$installText = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
Check ($installText -notmatch "Add-Check 'Visual C\+\+ runtime' 'fail'[^\r\n]*'vcruntime\.missing'") 'the preflight does not refuse a PC whose runtime is missing'
Check ($installText -match "Add-Check 'Visual C\+\+ runtime' 'warn'[^\r\n]*'vcruntime\.missing'") 'a missing runtime is a line that needs attention, not a refusal'
Check ($installText -match "'vcruntime\.no-installer'") 'the one refusal left is a package that does not carry the Microsoft installer'
Check ($installText -match 'Invoke-VcRuntimeInstall -Path \$vcExe') 'phase 2 runs the Microsoft installer'
$vcStep = [regex]::Match($installText, "(?s)Enter-Stage 'install' 'Phase 2: install'.{0,2400}")
Check ($vcStep.Success -and $vcStep.Value -match 'Invoke-VcRuntimeInstall' -and $vcStep.Value -match 'Test-VcRuntimeInstaller') 'the runtime step is the first of phase 2 and checks the file again before it runs it'
Check ($installText.IndexOf('Invoke-VcRuntimeInstall') -lt $installText.IndexOf("Enter-Stage 'files' 'Files'")) 'the runtime is installed before our own files are copied'
# What the old refusal left behind is gone: one reader of the three files, and no value that nothing reads.
Check (-not (Get-Command Get-VcRuntimeMissing -ErrorAction SilentlyContinue)) 'the reader of the old refusal is gone from common.ps1'
Check ($installText -notmatch 'VcRuntimeNeeded') 'the preflight keeps no value about the runtime that nothing reads'

'the shortcuts of the control application: Start menu and desktop, both by default (owner, 2026-10-12)'
$scAll = @(Get-ControlShortcutPaths)
Check ($scAll.Count -eq 3) "three shortcuts the installer knows ($($scAll.Count))"
Check ((@($scAll | Where-Object { $_.where -eq 'Start menu' }).Count -eq 2) -and (@($scAll | Where-Object { $_.where -eq 'desktop' }).Count -eq 1)) 'two in the Start menu (the application and its recovery view), one on the desktop'
Check (@($scAll | Where-Object { $_.kind -eq 'recovery' })[0].where -eq 'Start menu') 'the recovery entry stays in the Start menu only'
Check (@($scAll | Where-Object { $_.kind -eq 'recovery' })[0].arguments -eq '--recovery') 'the recovery entry carries its own argument'
Check (@($scAll | Where-Object { $_.where -eq 'desktop' })[0].path -match '[\\/]Desktop[\\/]amdgpu-wddm Control\.lnk$') "the desktop icon goes to the desktop of every user ($(@($scAll | Where-Object { $_.where -eq 'desktop' })[0].path))"
Check (@($scAll | ForEach-Object { $_.path } | Select-Object -Unique).Count -eq 3) 'the three paths differ'
# The name a line prints. The log and the dry run print the label alone: "$where $kind" read "desktop desktop icon"
# for the third row, which is the one line of this step a tester sees most often.
Check (@($scAll | Where-Object { $_.label }).Count -eq 3) 'every shortcut carries the name a person reads'
Check (@($scAll | ForEach-Object { $_.label } | Select-Object -Unique).Count -eq 3) 'the three names differ'
foreach ($sc in $scAll) {
    $words = @([string]$sc.label -split '\s+')
    Check ((@($words | Select-Object -Unique).Count -eq $words.Count)) "the name '$($sc.label)' says no word twice"
}
Check ((@($scAll | ForEach-Object { $_.label }) -join ', ') -eq 'Start menu entry, Start menu repair entry, desktop icon') "the summary line of the install reads '$((@($scAll | ForEach-Object { $_.label }) -join ', '))'"
$scBoth = @(Get-ControlShortcutPlan -NoStartMenu $false -NoDesktop $false)
Check ($scBoth.Count -eq 3) 'by default the install writes all three'
$scNoDesk = @(Get-ControlShortcutPlan -NoStartMenu $false -NoDesktop $true)
Check ($scNoDesk.Count -eq 2 -and -not (@($scNoDesk | Where-Object { $_.where -eq 'desktop' }).Count)) '-NoDesktopShortcut leaves the desktop icon out and keeps the Start menu'
$scNoMenu = @(Get-ControlShortcutPlan -NoStartMenu $true -NoDesktop $false)
Check ($scNoMenu.Count -eq 1 -and @($scNoMenu)[0].where -eq 'desktop') '-NoStartMenuShortcut leaves both Start menu entries out and keeps the desktop icon'
Check (@(Get-ControlShortcutPlan -NoStartMenu $true -NoDesktop $true).Count -eq 0) 'both switches together write no shortcut at all'
# A shortcut really written, read back, and removed again: the target, the arguments and the icon.
$scWork = Join-Path $work 'shortcuts'
[void][IO.Directory]::CreateDirectory($scWork)
$scTarget = Join-Path $scWork 'amdgpu_wddm_control.exe'
Write-Text $scTarget 'not a real program'
$scLink = Join-Path $scWork 'sub\amdgpu-wddm Control (recovery).lnk'
[void](New-ControlShortcut -Link $scLink -Target $scTarget -Arguments '--recovery')
$scRead = (New-Object -ComObject WScript.Shell).CreateShortcut($scLink)
Check ($scRead.TargetPath -eq $scTarget) 'a written shortcut points at the control application'
Check ($scRead.Arguments -eq '--recovery') 'it carries the arguments it was given'
Check ($scRead.IconLocation -like "$scTarget,0*") "it takes its icon from the application ($($scRead.IconLocation))"
Check ($scRead.WorkingDirectory -eq $scWork) 'its working directory is the application folder'
# The engine's own side: both switches survive a restart, the install writes by the plan, the uninstall removes all three.
$installText = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
$uninstallText = [IO.File]::ReadAllText((Join-Path $Installer 'uninstall.ps1'))
foreach ($switch in 'NoStartMenuShortcut', 'NoDesktopShortcut') {
    Check ($script:InstallSwitches -contains $switch) "-$switch is an install switch the state keeps over a restart"
    Check ($installText -match "\[switch\]\`$$switch") "install.ps1 takes -$switch"
}
Check ($installText -match 'Get-ControlShortcutPlan -NoStartMenu \(\[bool\]\$NoStartMenuShortcut\) -NoDesktop \(\[bool\]\$NoDesktopShortcut\)') 'the install writes the shortcuts of the plan'
Check ($installText -match 'New-ControlShortcut -Link \$link -Target \$controlExe') 'it writes them through the one writer'
Check ($installText -notmatch '\$\(\$_\.where\) \$\(\$_\.kind\)') 'no line joins the place and the kind into "desktop desktop icon" again'
Check ($installText -match 'shortcuts: \$\(if \(\$shortcutPlan\.Count\) \{ @\(\$shortcutPlan \| ForEach-Object \{ \$_\.label \}\)') 'the summary line prints the name of each shortcut'
Check ($installText -notmatch "Join-Path \`$env:ProgramData 'Microsoft\\\\Windows\\\\Start Menu") 'no second place in install.ps1 builds a shortcut path of its own'
Check ($uninstallText -match 'foreach \(\$sc in @\(Get-ControlShortcutPaths\)\)') 'the uninstall removes every shortcut the install can write'
Check ($uninstallText -notmatch "amdgpu-wddm Control \(recovery\)\.lnk'") 'the uninstall names no shortcut of its own any more'
# The switches an install carries over the restart that follows it.
$scState = [pscustomobject]@{ phase = 'testsigning-pending'; package_version = '1.0'; install_switches = @('NoDesktopShortcut') }
$scInputs = Get-InstallInputs -Bound @{} -State $scState -PackageVersion '1.0'
Check (@($scInputs.switches) -contains 'NoDesktopShortcut') 'the run after the restart leaves the desktop icon out again'
# The window states both answers (InstallFlow.ShortcutArguments), so a later run of the same install can put back a
# shortcut the first run left out: the positive switch clears the saved negative one.
$scInputs2 = Get-InstallInputs -Bound @{ NoStartMenuShortcut = $true; DesktopShortcut = $true } -State $scState -PackageVersion '1.0'
Check ((@($scInputs2.switches) -contains 'NoStartMenuShortcut') -and -not (@($scInputs2.switches) -contains 'NoDesktopShortcut')) 'a switch on the command line decides over the saved one'
$scInputs3 = Get-InstallInputs -Bound @{ StartMenuShortcut = $true; DesktopShortcut = $true } -State $scState -PackageVersion '1.0'
Check (-not (@($scInputs3.switches) -contains 'NoDesktopShortcut')) 'both places asked for again puts the desktop icon back'
foreach ($pair in @(@('NoStartMenuShortcut', 'StartMenuShortcut'), @('NoDesktopShortcut', 'DesktopShortcut'))) {
    $bad = $null
    try { [void](Get-InstallInputs -Bound @{ $pair[0] = $true; $pair[1] = $true } -State $scState -PackageVersion '1.0') }
    catch { $bad = $_.Exception.Message }
    Check ($bad -and $bad -match "-$($pair[0]) and -$($pair[1]) cannot be used together") "-$($pair[0]) with -$($pair[1]) is a usage error ($bad)"
}
foreach ($switch in 'StartMenuShortcut', 'DesktopShortcut') {
    Check ($installText -match "\[switch\]\`$$switch") "install.ps1 takes -$switch"
    Check (-not ($script:InstallSwitches -contains $switch)) "-$switch is not saved itself: it only clears the saved No switch"
}
Check ($installText -match '\$NoStartMenuShortcut = \[switch\]\(@\(\$installInputs\.switches\)') 'the install reads the shortcut answers back from the state'

'which Windows this driver is made for: n-2, and a line instead of a refusal (owner, 2026-10-11 and 2026-10-12)'
# Get-WindowsSupportFindings is pure, so every version, channel and edition is tested here and none of them needs a
# PC of its own. The severities are the whole contract: only 'fail' refuses the install.
function Get-WinIds($Findings) { return @(@($Findings) | ForEach-Object { $_.id }) }
function Get-WinSeverity($Findings, [string]$Id) { return [string](@(@($Findings) | Where-Object { $_.id -eq $Id })[0].severity) }
# The supported versions: 23H2, 24H2, 25H2, and a general build after them.
foreach ($b in 22631, 26100, 26200, 26500) {
    $w = Get-WindowsSupportFindings -Build $b -Caption 'Microsoft Windows 11 Pro'
    Check ((Get-WinIds $w.findings) -join ',' -eq 'windows.ok') "build $b is supported ($((Get-WinIds $w.findings) -join ','))"
    Check ($w.standing -eq 'supported') "build $b stands as supported"
}
# A Windows 11 below 23H2: one line that names the supported versions and advises an update, and the install goes on.
foreach ($b in 22000, 22621, 22630) {
    $w = Get-WindowsSupportFindings -Build $b -Caption 'Microsoft Windows 11 Pro' -DisplayVersion '22H2'
    Check ((Get-WinIds $w.findings) -join ',' -eq 'windows.below-supported') "build $b is a line, not a refusal ($((Get-WinIds $w.findings) -join ','))"
    Check ((Get-WinSeverity $w.findings 'windows.below-supported') -eq 'warn') "build ${b}: the line needs attention and refuses nothing"
    $d = [string]@($w.findings)[0].detail
    Check ($d -match '23H2' -and $d -match '24H2' -and $d -match '25H2') "build ${b}: the line names the supported versions ($d)"
    Check ($d -match '22631' -and $d -match '26100' -and $d -match '26200') "build ${b}: the line names their build numbers"
    Check ($d -match 'Update Windows') "build ${b}: the line advises an update"
    Check ($d -match '22H2') "build ${b}: the detail names the version this PC has"
}
# Windows 10 and older: the refusal that was there before stays as it is.
foreach ($b in 19045, 21999) {
    $w = Get-WindowsSupportFindings -Build $b -Caption 'Microsoft Windows 10 Pro'
    Check ((Get-WinIds $w.findings) -join ',' -eq 'windows.too-old') "build $b is refused ($((Get-WinIds $w.findings) -join ','))"
    Check ((Get-WinSeverity $w.findings 'windows.too-old') -eq 'fail') "build ${b}: the refusal is a failed check"
}
$w32 = Get-WindowsSupportFindings -Build 26100 -Caption 'Microsoft Windows 11 Pro' -Is64Bit $false
Check ((Get-WinIds $w32.findings) -join ',' -eq 'windows.not-x64') '32-bit Windows is refused and gets no second line'
Check ((Get-WinSeverity $w32.findings 'windows.not-x64') -eq 'fail') 'the 32-bit refusal is a failed check'
# An Insider build: a PC in the Insider programme that runs a build of its own, or a build number Microsoft has not
# released at all.
$wIns = Get-WindowsSupportFindings -Build 27891 -Caption 'Microsoft Windows 11 Pro' -SelfHostBranch 'Dev'
Check ((Get-WinIds $wIns.findings) -join ',' -eq 'windows.ok,windows.insider') "an Insider channel adds one line ($((Get-WinIds $wIns.findings) -join ','))"
Check ((Get-WinSeverity $wIns.findings 'windows.insider') -eq 'warn') 'the Insider line refuses nothing'
Check ([string](@($wIns.findings | Where-Object { $_.id -eq 'windows.insider' })[0].detail) -match 'Dev') 'the Insider line names the channel'
$wCan = Get-WindowsSupportFindings -Build 27891 -Caption 'Microsoft Windows 11 Pro'
Check ((Get-WinIds $wCan.findings) -contains 'windows.insider') 'a build at or above 27000 is an Insider build'
# The channel alone is not the signal: a PC enrolled in the Insider programme (Release Preview above all) that runs
# one of the three supported builds runs exactly what this driver is tested on, and reads nothing about Insider.
foreach ($b in 22631, 26100, 26200) {
    foreach ($branch in 'ReleasePreview', 'RP', 'Beta') {
        $wRp = Get-WindowsSupportFindings -Build $b -Caption 'Microsoft Windows 11 Pro' -SelfHostBranch $branch
        Check ((Get-WinIds $wRp.findings) -join ',' -eq 'windows.ok') "$branch on the supported build $b adds no line ($((Get-WinIds $wRp.findings) -join ','))"
    }
}
# The same enrolment on a build that is not one of them keeps the line.
$wRpOld = Get-WindowsSupportFindings -Build 22621 -Caption 'Microsoft Windows 11 Pro' -SelfHostBranch 'ReleasePreview'
Check ((Get-WinIds $wRpOld.findings) -join ',' -eq 'windows.below-supported,windows.insider') "an enrolled PC on build 22621 reads both lines ($((Get-WinIds $wRpOld.findings) -join ','))"
# Negative control: 26300 is a build a tester had (BD-125), not an Insider build, and no line is added for it.
$w26300 = Get-WindowsSupportFindings -Build 26300 -Caption 'Microsoft Windows 11 Pro'
Check ((Get-WinIds $w26300.findings) -join ',' -eq 'windows.ok') "build 26300 adds no line ($((Get-WinIds $w26300.findings) -join ','))"
# The LTSC editions: not tested, and that is all.
foreach ($ed in 'EnterpriseS', 'IoTEnterpriseS') {
    $w = Get-WindowsSupportFindings -Build 26100 -Caption 'Microsoft Windows 11 Enterprise LTSC' -EditionId $ed
    Check ((Get-WinIds $w.findings) -join ',' -eq 'windows.ok,windows.ltsc') "$ed adds one line ($((Get-WinIds $w.findings) -join ','))"
    Check ((Get-WinSeverity $w.findings 'windows.ltsc') -eq 'warn') "$ed refuses nothing"
    Check ([string](@($w.findings | Where-Object { $_.id -eq 'windows.ltsc' })[0].detail) -match $ed) "the $ed line names the edition"
}
# Negative controls: the ordinary editions add no line of their own.
foreach ($ed in 'Professional', 'Enterprise', 'Core', 'IoTEnterprise') {
    $w = Get-WindowsSupportFindings -Build 26100 -Caption 'Microsoft Windows 11' -EditionId $ed
    Check ((Get-WinIds $w.findings) -join ',' -eq 'windows.ok') "edition $ed adds no line ($((Get-WinIds $w.findings) -join ','))"
}
# A refused Windows gets the refusal alone: nothing is going to be installed there.
$wOld = Get-WindowsSupportFindings -Build 19045 -Caption 'Microsoft Windows 10 Enterprise LTSC' -EditionId 'EnterpriseS' -SelfHostBranch 'Beta'
Check ((Get-WinIds $wOld.findings) -join ',' -eq 'windows.too-old') "an old Windows is refused once ($((Get-WinIds $wOld.findings) -join ','))"
# Both lines at once, on one PC: the Install button is not held back by either.
$wBoth = Get-WindowsSupportFindings -Build 22621 -Caption 'Microsoft Windows 11 Enterprise LTSC' -EditionId 'EnterpriseS' -SelfHostBranch 'Beta'
Check ((Get-WinIds $wBoth.findings) -join ',' -eq 'windows.below-supported,windows.insider,windows.ltsc') "three lines together ($((Get-WinIds $wBoth.findings) -join ','))"
Check (@(@($wBoth.findings) | Where-Object { $_.severity -eq 'fail' }).Count -eq 0) 'none of the three lines refuses the install'
# The running PC reads the same way (this PC is a supported or a newer Windows).
$wHere = Get-WindowsSupportState
Check ($wHere -and $wHere.build -gt 0) "the running PC reads its own standing: $($wHere.standing), build $($wHere.build)"
Check (@(@($wHere.findings) | Where-Object { $_.severity -eq 'fail' }).Count -eq 0) 'this PC is not refused by the policy'
# install.ps1's side: the ids, and no refusal for the three information lines.
foreach ($id in 'windows.below-supported', 'windows.insider', 'windows.ltsc') {
    Check ($installText -match "Add-Check '[^']+' 'warn' \`$f\.detail '$([regex]::Escape($id))'") "the preflight states $id as a line that needs attention"
    Check ($installText -notmatch "'fail'[^\r\n]*'$([regex]::Escape($id))'") "the preflight does not refuse $id"
}
foreach ($id in 'windows.not-x64', 'windows.too-old') {
    Check ($installText -match "Add-Check 'Windows' 'fail' \`$f\.detail '$([regex]::Escape($id))'") "the preflight still refuses $id"
}
# A finding id the preflight's list does not know keeps its own severity: a line that refuses the install must not
# read as a passed check because the list was not extended with it.
$winDefault = [regex]::Match($installText, "(?s)default \{\s*\r?\n\s*if \(\[string\]\`$f\.severity -eq 'fail'\).{0,400}?\n\s*\}")
Check ($winDefault.Success -and $winDefault.Value -match "Add-Check 'Windows' 'fail'" -and $winDefault.Value -match "Add-Check 'Windows' 'warn'") 'an unknown Windows finding keeps its severity in the preflight'
Check ($installText -notmatch "default \{ Add-Check 'Windows' 'ok' \`$f\.detail 'windows\.ok' \}") 'nothing turns an unknown Windows finding into a passed check'
Check ($installText -match '\$win = Get-WindowsSupportState -Os \$os') 'the preflight asks the one place that decides'
Check ($installText -notmatch '\$build -lt 22000') 'no version rule is left in install.ps1 itself'

'the installer and the uninstaller ask nothing (owner, 2026-10-12)'
# The function that asked is gone from the engine, so a new question cannot be added by calling it, and no script of
# the installer calls one.
$common = [IO.File]::ReadAllText((Join-Path $Installer 'common.ps1'))
Check ($common -notmatch 'function Read-Confirmation') 'the engine has no function that asks the user a question'
foreach ($name in 'install.ps1', 'uninstall.ps1', 'common.ps1', 'components.ps1', 'prepare-offline.ps1', 'system-tuning.ps1') {
    $text = [IO.File]::ReadAllText((Join-Path $Installer $name))
    Check ($text -notmatch '(?m)^[^#]*Read-Confirmation') "$name asks no question"
}
# The two scripts a person starts: neither stops for an answer. (components.ps1 still offers the console install its
# list of extras and models, with a default on Enter: that is a choice between good outcomes, not a permission.)
foreach ($name in 'install.ps1', 'uninstall.ps1') {
    $text = [IO.File]::ReadAllText((Join-Path $Installer $name))
    Check ($text -notmatch '(?m)^[^#]*Read-Host') "$name reads no answer from the console"
}
$uninstall = [IO.File]::ReadAllText((Join-Path $Installer 'uninstall.ps1'))
Check ($uninstall -match 'Wait-RestartCountdown -Seconds \$script:RestartCountdownSeconds') 'the uninstaller restarts after the same visible countdown as the installer'
Check ($uninstall -match '-KeepTestSigning') 'test signing goes off again by itself, and a switch leaves it on'
Check ($uninstall -match "Join-Path \`$dataRoot 'setup'") 'the uninstaller removes what a single-file setup unpacked'

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
