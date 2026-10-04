# Host unit test of the engine parts that the setup window relies on (GUI plan A1, C7, F-VER), Windows PowerShell 5.1
# like the installer:
#   powershell -NoProfile -File tools\release\test-engine-units.ps1 [-Installer <package>\installer] [-WorkRoot <dir>]
# G-STAGE: the RunOnce command line (Format-CommandLine, parsed back through CommandLineToArgvW), the continuation
# closure (staged once, checked, a changed copy repaired, a damaged source refused), the continuation command of the
# setup window and of the command line, and the kept repair set (active + previous, older removed, firmware completed).
# Witness writer (G-VER, writer side): the record binds the loaded image, the reply and the boot; the reader's rule.
# Compatibility record (C7, used by G-RB from Ph 3): a compliant record verifies; the tester.10 shape (no Reboot
# directive), a missing or unknown record, another firmware set, an incomplete firmware folder and another engine
# contract are refused. The engine lock: one mutating engine at a time.
# Everything is written under -WorkRoot; nothing in the registry, no driver, no restart.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$WorkRoot = (Join-Path (Split-Path (Split-Path $Installer)) 'test-tmp'))
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
    param([string]$Dir, [string]$Version = '0.7.199.100-test.1', [string]$Inf = $infGood, [switch]$NoRecord, [scriptblock]$EditRecord, $Firmware = $fwPin, [AllowNull()][AllowEmptyString()][string]$RecordText = $null, [switch]$RawRecord)
    Write-Text (Join-Path $Dir 'payload\kmd\bc250kmd.inf') $Inf
    Write-Text (Join-Path $Dir 'payload\kmd\bc250kmd.sys') "kmd image $Version"
    Write-Text (Join-Path $Dir 'installer\registry-defaults.json') '{ "schema": 1, "defaults": { "parameters": { "DpmMode": 1 }, "desktop_router": { "DwmForceCpu": 0 } } }'
    Write-Text (Join-Path $Dir 'install.cmd') '@echo off'
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

'[G-STAGE] continuation command: the setup window of the closure, else Windows PowerShell with the closure''s install.ps1; verify from the install root'
$psExe = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$c = Get-ContinuationCommand -Kind continue -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $true
Check (($c.exe -eq $psExe) -and ((@($c.arguments) -join ' ') -eq "-NoProfile -ExecutionPolicy Bypass -File $closure\installer\install.ps1 -HoldWindow")) 'GUI run, closure without a setup window: Windows PowerShell runs the closure''s install.ps1 directly (no cmd.exe)'
Write-Text (Join-Path $closure $script:SetupExeRelative) 'exe'
$c = Get-ContinuationCommand -Kind continue -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $true
Check (($c.exe -eq (Join-Path $closure $script:SetupExeRelative)) -and ((@($c.arguments) -join ' ') -eq '--continue')) 'GUI run: the closure''s setup window with --continue'
$c = Get-ContinuationCommand -Kind continue -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $false
Check ((@($c.arguments) -join ' ') -eq "-NoProfile -ExecutionPolicy Bypass -File $closure\installer\install.ps1 -HoldWindow") 'command-line run: the closure''s install.ps1, never the original folder'
$c = Get-ContinuationCommand -Kind verify -Closure $closure -InstallRoot 'C:\Program Files\amdgpu-wddm' -Gui $false
$line = Format-CommandLine $c.exe $c.arguments
$argv = [EngineUnits.Argv]::Split($line)
Check (($argv[0] -eq $psExe) -and ($argv[5] -eq 'C:\Program Files\amdgpu-wddm\installer\install.ps1') -and (($argv[6..7] -join ' ') -eq '-HoldWindow -Verify')) "verify: $line"
Remove-Item -LiteralPath (Join-Path $closure 'setup') -Recurse -Force

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

'[G-EVT] settings-impact rows from the registry-default plans'
$plan = Get-RegistryDefaultPlan -Defaults ([pscustomobject]@{ DpmMode = 1; DpmMaxMHz = 1500; EnableFullWddm = 2 }) -Previous ([pscustomobject]@{ DpmMaxMHz = 1400; EnableFullWddm = 2 }) -Current @{ DpmMaxMHz = 1400; EnableFullWddm = 1 } -Explicit @{} -Owned ([ordered]@{ UnconfirmedStarts = 0 })
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
$why = Write-RunningReleaseWitness -InstallRoot $pkg -RecordedBy 'verify' -Boot $boot -Path $wpath
Check (($why -match 'no bc250kmd\.sys among the loaded drivers|no driver reply') -and -not (Test-Path -LiteralPath $wpath)) "on this PC (no driver): nothing written ($why)"
$why = Write-RunningReleaseWitness -InstallRoot (Join-Path $work 'nowhere') -RecordedBy 'start-confirm' -Path $wpath
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

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
