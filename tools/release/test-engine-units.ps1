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
    @{ p = 'install.cmd'; want = $null }                                   # read from the package, never copied
    @{ p = 'INSTALL.md'; want = $null }
    @{ p = 'setup/amdgpu_wddm_setup.exe'; want = $null }                   # the setup window runs from the package
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

$pnputil = @"
Microsoft PnP Utility

Published Name:     oem9.inf
Original Name:      bc250kmd.inf
Provider Name:      amdgpu-wddm
Class Name:         Display adapters
Driver Version:     10/02/2026 0.7.198.100
Signer Name:        amdgpu-wddm test

Published Name:     oem12.inf
Original Name:      bc250kmd.inf
Provider Name:      amdgpu-wddm
Class Name:         Display adapters
Driver Version:     10/04/2026 0.7.208.100
Signer Name:        amdgpu-wddm test

Published Name:     oem13.inf
Original Name:      nvidia.inf
Provider Name:      NVIDIA
Class Name:         Display adapters
Driver Version:     09/01/2026 32.0.15.6094
Signer Name:        Microsoft Windows Hardware Compatibility Publisher

Published Name:     oem15.inf
Original Name:      bc250kmd.inf
Provider Name:      amdgpu-wddm
Class Name:         Display adapters
Driver Version:     10/06/2026 0.7.213.101
Signer Name:        amdgpu-wddm test
"@
$rowsPnp = @(Get-OurDriverPackageRows $pnputil)
Check (((@($rowsPnp | ForEach-Object { "$($_.published)=$($_.version)" }) -join ', ') -eq 'oem9.inf=0.7.198.100, oem12.inf=0.7.208.100, oem15.inf=0.7.213.101')) "pnputil /enum-drivers: only our packages, each with its version ($(@($rowsPnp | ForEach-Object { $_.published }) -join ', '))"
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
$saveStateDir = $script:StateDir
$script:StateDir = Join-Path $work 'footprint-state\installer'
$fpRoot = Join-Path $work 'footprint\install-root'
$saveProfiles = $script:ProfilePathsOverride
$script:ProfilePathsOverride = @((Join-Path $work 'footprint\profiles\a'), (Join-Path $work 'footprint\profiles\b'))
$fp = @(Get-ReleaseFootprint -InstallRoot $fpRoot -State ([pscustomobject]@{ firmware_dir_existed = $true }) -MftKeys @('HKLM:\SOFTWARE\Classes\CLSID\{test}'))
$items = @($fp | ForEach-Object { $_.item })
$wantItems = @('install root', 'System32 stub', 'SysWOW64 stub', 'driver store', 'driver service', 'policy keys', 'Vulkan registration', 'H.264 encoder keys', 'scheduled task', 'RunOnce entry', 'Start menu', 'installer state', 'per-user data', 'certificates', 'GPU firmware', 'control application data')
Check ((($items -join ' | ') -eq ($wantItems -join ' | '))) "every item of the release is checked: $($items -join ', ')"
Check (-not @($fp | Where-Object { $_.detail -match '^not read' }).Count) "every probe read its item$(if (@($fp | Where-Object { $_.detail -match '^not read' }).Count) { ': ' + (@($fp | Where-Object { $_.detail -match '^not read' } | ForEach-Object { "$($_.item) $($_.detail)" }) -join '; ') })"
$byItem = @{}; foreach ($row in $fp) { $byItem[$row.item] = $row }
Check ((-not $byItem['install root'].present) -and ($byItem['install root'].detail -match 'is gone')) 'an install root that is not there is gone'
Check ((@($byItem['H.264 encoder keys']).present) -and ($byItem['H.264 encoder keys'].detail -match '\{test\}')) 'the encoder keys come from the caller (uninstall.ps1 reads them with the classes key it uses)'
Check (($byItem['GPU firmware'].kept) -and ($byItem['control application data'].kept) -and -not @($fp | Where-Object { $_.kept -and $_.item -notin @('GPU firmware', 'control application data') }).Count) 'only the firmware folder that was there before the install and the control data are kept on purpose'
Write-Text (Join-Path $fpRoot 'manifest.json') '{}'
Write-Text (Join-Path $script:StateDir 'state.json') '{}'
$fp2 = @(Get-ReleaseFootprint -InstallRoot $fpRoot -State $null)
$byItem2 = @{}; foreach ($row in $fp2) { $byItem2[$row.item] = $row }
Check ($byItem2['install root'].present -and ($byItem2['install root'].detail -match 'is there')) 'an install root that is there is reported as there'
Check ($byItem2['installer state'].present -and -not $byItem2['installer state'].kept) 'the installer state folder is reported, and it is not a kept item'
Check (-not $byItem2['GPU firmware'].kept) 'a firmware folder that the install created itself is not a kept item'
Check (-not @($fp2 | Where-Object { $_.item -eq 'H.264 encoder keys' } | Where-Object { $_.present }).Count) 'no encoder key given: the row says none'
Check ((-not $byItem['per-user data'].present) -and ($byItem['per-user data'].detail -match 'in any profile')) 'no profile with our folder: the per-user row is gone'
$udB = Join-Path $work 'footprint\profiles\b\AppData\Local\amdgpu-wddm'
Write-Text (Join-Path $udB 'vkd3d\x.cache') 'x'
$ud = @(Get-OurUserDataDirs)
Check (($ud.Count -eq 1) -and ($ud[0] -eq $udB)) "one profile with our folder: exactly that folder is found ($($ud -join ', '))"
$fp3 = @(Get-ReleaseFootprint -InstallRoot $fpRoot -State $null)
Check (@($fp3 | Where-Object { $_.item -eq 'per-user data' -and $_.present -and -not $_.kept }).Count -eq 1) 'per-user data that is there is reported as LEFT, not kept'
Remove-PathOrSchedule $udB
Check ((-not (Test-Path -LiteralPath $udB)) -and (-not @(Get-OurUserDataDirs).Count)) 'the uninstaller removal takes the per-user folder away'
$script:ProfilePathsOverride = $saveProfiles
$script:StateDir = $saveStateDir

$saveStateDir = $script:StateDir
$script:StateDir = Join-Path $work 'bd089-state'
Write-Text (Join-Path (Get-ClosureDir '0.7.208.100-tester.13') 'manifest.json') '{ "version": "0.7.208.100-tester.13", "kmd_version": "0.7.208.100" }'
Write-Text (Join-Path (Get-ClosureDir 'damaged') 'manifest.json') 'not a manifest'
Check (((Get-RepairSetDriverVersion '0.7.208.100-tester.13') -eq '0.7.208.100') -and ($null -eq (Get-RepairSetDriverVersion 'damaged')) -and ($null -eq (Get-RepairSetDriverVersion '0.7.199.100-tester.11')) -and ($null -eq (Get-RepairSetDriverVersion ''))) 'the driver version of a kept repair set comes from its own manifest; a damaged or missing set has none'
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

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
exit 0
