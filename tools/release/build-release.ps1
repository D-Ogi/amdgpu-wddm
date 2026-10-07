# Builds the tester release package: one folder and its zip that a tester unpacks and runs (install.cmd).
#
#   pwsh -File tools\release\build-release.ps1 [-Root <BC250_ROOT>] [-Out <dir>] [-Version <v>]
#
# Inputs are the artifacts listed in release-sources.json (registered lab drivers, control application, tools,
# licence texts), each refused unless its SHA256 matches. The AMD GPU firmware is not packaged: manifest.json carries
# its pinned list (tools/firmware/cyan_skillfish2.json) and the installer downloads it. The KMD package is re-signed with the release test certificate
# (new-release-cert.ps1, private key in a private directory outside the repository, never copied): the .sys gets the release
# signature in place of the lab one (its code and its Authenticode hash do not change), Inf2Cat makes a new catalog,
# and the catalog is signed. The package holds the public .cer only.
# manifest.json lists every file of the package with its SHA256, and every installed component with its role,
# install location, file version and SHA256 (the control application reads it from the install root).
# Gates: the work ledger (every check rule, plus no finished but unlanded work for a component of this release;
# override with -AllowLedgerDebt -LedgerDebtReason, or -NoLedger -NoLedgerReason in a workspace without the
# ledger; manifest.json records which of the three happened), every source hash, signer of .sys and .cat = the release certificate, no private-key material in the
# package, every installer script parses under Windows PowerShell 5.1, every form of cli-commands.json answered by the
# packaged CLI, the marker guard (no fix lost its comment on the way into this release: test-marker-guard.ps1) and the
# kernel compile (driver\kmd and driver\shim build with the WDK of this workspace: test-kmd-compile.ps1). Nothing here
# opens a window: child processes run with CreateNoWindow and redirected output.
[CmdletBinding()]
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [string]$Out,
    [string]$DriverVer = '0.7.216.100',          # the release's own 4th field: ranks above the lab's x.y.z.1, names the package
    [string]$Version = '0.7.216.100-tester.20',
    [string]$KitVersion = '10.0.26100.0',
    [string]$SetupApp,                           # optional: the built setup window (tools\win\amdgpu_wddm_setup\build.ps1 output), copied to setup\
    [switch]$AllowLedgerDebt,                    # build although finished work for a release component is unlanded
    [string]$LedgerDebtReason,                   # required with -AllowLedgerDebt: recorded in the build log and manifest
    [switch]$NoLedger,                           # build in a workspace that has no work ledger at all
    [string]$NoLedgerReason                      # required with -NoLedger: recorded in the build log and manifest
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
if (-not $Out) { $Out = Join-Path $Root 'scratch\release\out' }
$repo = Split-Path (Split-Path $PSScriptRoot)
$name = "amdgpu-wddm-tester-$Version"
$pkg = Join-Path $Out $name
$zip = "$pkg.zip"
$sources = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-sources.json') -Raw | ConvertFrom-Json
$secrets = Join-Path $Root 'secrets\release'
$pfx = Join-Path $secrets 'amdgpu-wddm-release.pfx'
$cer = Join-Path $secrets 'amdgpu-wddm-release.cer'
$passFile = Join-Path $secrets 'amdgpu-wddm-release.pass'
foreach ($f in $pfx, $cer, $passFile) { if (-not (Test-Path -LiteralPath $f)) { throw "missing ${f}: run tools\release\new-release-cert.ps1 once" } }
$wdk = Join-Path $Root 'toolchain\nuget\microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$signtool = Join-Path $sdk "bin\$KitVersion\x64\signtool.exe"
$inf2cat = Join-Path $wdk "bin\$KitVersion\x86\Inf2Cat.exe"
foreach ($t in $signtool, $inf2cat) { if (-not (Test-Path -LiteralPath $t)) { throw "missing tool $t" } }

# Work ledger gate: a release must not ship a component whose finished fix is still unlanded,
# and must not be cut over a ledger that hides new work or claims a landing nobody can verify
# (the checker runs every rule in release mode). The ledger is a local file of the workspace
# (BC250_ROOT), so none of its contents reach the package or the public repository.
# The gate is never skipped silently: without the checker the build stops unless -NoLedger
# states why, and a failing gate needs -AllowLedgerDebt with a reason. manifest.json records
# which of the three happened, so the package itself says whether it was gated.
$ledgerTool = Join-Path $Root 'scratch\ledger\ledger.py'
$script:ledgerGate = 'enforced'
$script:ledgerGateReason = ''
if (-not (Test-Path -LiteralPath $ledgerTool)) {
    if (-not $NoLedger) {
        throw ("work ledger: no $ledgerTool in this workspace (BC250_ROOT=$Root). The gate is " +
               "required: point -Root at the workspace, or build with -NoLedger -NoLedgerReason '<why>'.")
    }
    if (-not $NoLedgerReason) { throw '-NoLedger needs -NoLedgerReason "<why this build has no ledger>"' }
    $script:ledgerGate = 'absent'
    $script:ledgerGateReason = $NoLedgerReason
    Write-Host "work ledger: NO LEDGER in this workspace, gate skipped by -NoLedger"
    Write-Host "work ledger: reason: $NoLedgerReason"
} else {
    Write-Host "work ledger: checking $Version against the release components"
    $ledgerOut = & python $ledgerTool --root $Root check --release $Version 2>&1
    $ledgerExit = $LASTEXITCODE
    $ledgerOut | ForEach-Object { "  $_" }
    if ($ledgerExit -gt 1) {
        # Exit 2 is a broken or missing ledger, not debt: -AllowLedgerDebt must not wave it through.
        throw ("work ledger: the checker failed with exit $ledgerExit (a malformed or missing " +
               "LEDGER.json, or no python). Fix the ledger: this is not release debt.")
    }
    if ($ledgerExit -ne 0) {
        if (-not $AllowLedgerDebt) {
            throw ("work ledger: finished but unlanded work serves a component of this release, or the " +
                   "ledger itself is inconsistent (see the lines above). Land it, or build with " +
                   "-AllowLedgerDebt -LedgerDebtReason '<why>'.")
        }
        if (-not $LedgerDebtReason) {
            throw '-AllowLedgerDebt needs -LedgerDebtReason "<why this release ships with unlanded work>"'
        }
        $script:ledgerGate = 'debt-accepted'
        $script:ledgerGateReason = $LedgerDebtReason
        Write-Host "work ledger: DEBT ACCEPTED for $Version by -AllowLedgerDebt"
        Write-Host "work ledger: reason: $LedgerDebtReason"
        Write-Host "work ledger: accepted at $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')) UTC"
    } else {
        Write-Host 'work ledger: clean: no unlanded work for any component of this release'
    }
}

if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
[void][IO.Directory]::CreateDirectory($pkg)

Write-Host "sources ($(@($sources.files).Count))"
foreach ($f in $sources.files) {
    $src = Join-Path $Root ($f.source -replace '/', '\')
    $h = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
    if ($h -ne $f.sha256) { throw "source hash mismatch: $($f.source) is $h, release-sources.json says $($f.sha256)" }
    $dst = Join-Path $pkg ($f.path -replace '/', '\')
    [void][IO.Directory]::CreateDirectory((Split-Path $dst))
    Copy-Item -LiteralPath $src -Destination $dst
    '  {0}  {1,-12} {2}' -f $h.Substring(0, 8), $f.component, $f.path
}
# The D3D11 shell refuses an engine or ICD whose SHA-256 differs from its caps record (adapter-config.h,
# AdapterConfigRecord2: engine_sha256 at byte 68, icd_sha256 at byte 100) before it loads them. b15 shipped a new ICD
# with the old record and every D3D11 device failed (ERROR_CRC): check each record against its siblings here.
foreach ($cfg in Get-ChildItem -LiteralPath (Join-Path $pkg 'payload') -Recurse -File -Filter 'amdgpu_wddm_d3d11.config') {
    $raw = [IO.File]::ReadAllBytes($cfg.FullName)
    if ($raw.Length -ne 132 -or [BitConverter]::ToUInt32($raw, 0) -ne 0x4334314D) { throw "$($cfg.FullName): not a 132-byte M14C caps record" }
    foreach ($pin in @(@{ name = 'amdgpu_wddm_dxvk.dll'; at = 68 }, @{ name = 'amdgpu_wddm_radv.dll'; at = 100 })) {
        $want = -join ($raw[$pin.at..($pin.at + 31)] | ForEach-Object { $_.ToString('X2') })
        $have = (Get-FileHash -LiteralPath (Join-Path $cfg.DirectoryName $pin.name) -Algorithm SHA256).Hash
        if ($want -ne $have) { throw "$($cfg.FullName) pins $($pin.name) $($want.Substring(0, 8)), the package has $($have.Substring(0, 8)): regenerate the record (tools/build/write-umd-config.py)" }
    }
    '  caps record {0} pins its engine and ICD' -f $cfg.FullName.Substring($pkg.Length + 1)
}

Write-Host 'installer and documents'
# The start-confirm task's scripts: it also writes the running-release witness (release-witness.ps1).
$taskScripts = @('start-confirm.ps1', 'start-confirm-core.ps1', 'dwm-session.ps1', 'release-witness.ps1')
$inst = Join-Path $pkg 'installer'
[void][IO.Directory]::CreateDirectory($inst)
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'installer') -File) {
    if ($f.Extension -eq '.cmd') { Copy-Item -LiteralPath $f.FullName -Destination $pkg } else { Copy-Item -LiteralPath $f.FullName -Destination $inst }
}
# The installed copy of the start-confirm task runs from <install root>\tools next to bc250kmd_cli.exe.
foreach ($f in $taskScripts) { Copy-Item -LiteralPath (Join-Path $inst $f) -Destination (Join-Path $pkg "payload\tools\$f") }
Copy-Item -LiteralPath (Join-Path $repo 'docs\testing\INSTALL.md') -Destination (Join-Path $pkg 'INSTALL.md')
# TESTERS.md is not shipped any more. It named one package version of its own ("0.7.199.100-tester.11") in every
# package up to tester.12, it read as the tester-facing note, and no gate held it to the release that carried it. The
# per-release RELEASE-NOTES.md below replaced it.
# Release notes (GUI plan WU-044, C16): docs/testing/release-notes/<version>.md in English, with the four sections of the
# release checklist; the setup window shows them before any change. A translation <version>.<lang>.md goes in as
# RELEASE-NOTES.<lang>.md; without one the window shows the English text, labelled as English.
$notesDir = Join-Path $repo 'docs\testing\release-notes'
$notes = Join-Path $notesDir "$Version.md"
if (-not (Test-Path -LiteralPath $notes)) { throw "no release notes for ${Version}: $notes" }
$notesText = Get-Content -LiteralPath $notes -Raw
foreach ($h in 'New', 'Fixed', 'Known issues', 'Settings affected') {
    if ($notesText -notmatch ('(?m)^## ' + [regex]::Escape($h) + '\s*$')) { throw "release notes $notes lack the section '## $h'" }
}
Copy-Item -LiteralPath $notes -Destination (Join-Path $pkg 'RELEASE-NOTES.md')
foreach ($t in Get-ChildItem -LiteralPath $notesDir -File -Filter "$Version.*.md") {
    $lang = $t.Name.Substring($Version.Length + 1) -replace '\.md$', ''
    if ($lang -notmatch '^[a-z]{2}$') { throw "release notes translation $($t.Name): expected <version>.<two-letter language>.md" }
    Copy-Item -LiteralPath $t.FullName -Destination (Join-Path $pkg "RELEASE-NOTES.$lang.md")
}
'  release notes: RELEASE-NOTES.md{0}' -f $(if (@(Get-ChildItem -LiteralPath $pkg -Filter 'RELEASE-NOTES.*.md').Count) { ' + ' + ((Get-ChildItem -LiteralPath $pkg -Filter 'RELEASE-NOTES.*.md' | ForEach-Object { $_.Name }) -join ', ') } else { '' })
# The setup window (optional until it ships): setup\amdgpu_wddm_setup.exe (its string tables are embedded). The installer's
# continuation runs it from the staged closure after each restart that a setup-window run asked for.
if ($SetupApp) {
    $setupExe = Join-Path $SetupApp 'amdgpu_wddm_setup.exe'
    if (-not (Test-Path -LiteralPath $setupExe)) { throw "-SetupApp ${SetupApp}: no amdgpu_wddm_setup.exe" }
    $setupDst = Join-Path $pkg 'setup'
    [void][IO.Directory]::CreateDirectory($setupDst)
    foreach ($f in Get-ChildItem -LiteralPath $SetupApp -File | Where-Object { $_.Extension -in '.exe', '.config' }) { Copy-Item -LiteralPath $f.FullName -Destination $setupDst }
    '  setup window: {0} ({1})' -f $setupExe, (Get-FileHash -LiteralPath $setupExe -Algorithm SHA256).Hash.Substring(0, 8)
}
foreach ($f in 'LICENSE.md', 'NOTICE') { Copy-Item -LiteralPath (Join-Path $repo $f) -Destination $pkg }
# The release's own third-party list (each bundled file and its licence file) and the licence texts, each from the
# source that release-sources.json records (repository, path, commit) and refused unless its SHA256 matches.
Copy-Item -LiteralPath (Join-Path $repo 'docs\testing\THIRD-PARTY.md') -Destination (Join-Path $pkg 'THIRD-PARTY.md')
$lic = Join-Path $pkg 'licenses'
[void][IO.Directory]::CreateDirectory($lic)
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE.md') -Destination (Join-Path $lic 'amdgpu-wddm-LICENSE.md')
Copy-Item -LiteralPath (Join-Path $repo 'NOTICE') -Destination (Join-Path $lic 'amdgpu-wddm-NOTICE.txt')
foreach ($l in $sources.licenses) {
    $src = Join-Path $repo ($l.source -replace '/', '\')
    $h = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
    if ($h -ne $l.sha256) { throw "licence text $($l.source) is $h, release-sources.json says $($l.sha256) ($($l.repository) $($l.path) @ $($l.ref))" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $lic $l.file)
    '  {0}  licence      {1}  ({2} {3} @ {4})' -f $h.Substring(0, 8), $l.file, $l.repository, $l.path, $l.ref
}
$named = @([regex]::Matches((Get-Content -LiteralPath (Join-Path $pkg 'THIRD-PARTY.md') -Raw), '`([\w.-]+\.(?:txt|md|rst)|LICENSE\.amdgpu)`') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
foreach ($n in $named) { if (-not (Test-Path -LiteralPath (Join-Path $lic $n))) { throw "THIRD-PARTY.md names licenses\$n, which is missing" } }
foreach ($f in Get-ChildItem -LiteralPath $lic -File) { if ($named -notcontains $f.Name) { throw "licenses\$($f.Name) is not named in THIRD-PARTY.md" } }
# Every payload file has its row in THIRD-PARTY.md.
foreach ($f in $sources.files) {
    $leaf = ($f.path -replace '^payload/', '') -replace '/', '\'
    if ((Get-Content -LiteralPath (Join-Path $pkg 'THIRD-PARTY.md') -Raw) -notmatch [regex]::Escape("``$leaf``")) { throw "THIRD-PARTY.md has no row for $($f.path)" }
}
# No shipped document may name a package version other than this one: TESTERS.md named 0.7.199.100-tester.11 in every
# package up to tester.12, and it read as the tester-facing note. This runs after the last document is copied, so it
# covers every one of them. The release notes are the one exception: they are chosen by version above, and they name
# an earlier release where a tester has to know about it.
foreach ($d in Get-ChildItem -LiteralPath $pkg -File -Filter '*.md') {
    if ($d.Name -like 'RELEASE-NOTES*') { continue }
    $versionNames = @([regex]::Matches((Get-Content -LiteralPath $d.FullName -Raw), '\d+\.\d+\.\d+\.\d+-tester\.\d+') | ForEach-Object { $_.Value } | Sort-Object -Unique | Where-Object { $_ -ne $Version })
    if ($versionNames.Count) { throw "$($d.Name) names the package version $($versionNames -join ', '): a shipped document may name only this package's version ($Version), or no version at all" }
}
'  documents: {0} name no package version but this one ({1})' -f @(Get-ChildItem -LiteralPath $pkg -File -Filter '*.md' | Where-Object { $_.Name -notlike 'RELEASE-NOTES*' }).Count, ((Get-ChildItem -LiteralPath $pkg -File -Filter '*.md' | Where-Object { $_.Name -notlike 'RELEASE-NOTES*' } | ForEach-Object { $_.Name }) -join ', ')

# The AMD firmware is not part of the package: manifest.json carries the pinned list (tools/firmware) and the
# installer downloads the files at install time.
$pin = Get-Content -LiteralPath (Join-Path $repo ($sources.firmware.pinned_list -replace '/', '\')) -Raw | ConvertFrom-Json
if ($pin.commit -ne $sources.firmware.commit -or $sources.firmware.commit -notmatch '^[0-9a-f]{40}$') { throw "firmware commit: pinned list $($pin.commit), release-sources.json $($sources.firmware.commit)" }
$fwLicence = (Get-FileHash -LiteralPath (Join-Path $lic $sources.firmware.licence.name)).Hash
if ($fwLicence -ne $sources.firmware.licence.sha256) { throw "licenses\LICENSE.amdgpu is $fwLicence, release-sources.json firmware.licence says $($sources.firmware.licence.sha256)" }
$fwFiles = @(foreach ($f in $pin.files) { [ordered]@{ name = $f.name; path = "amdgpu/$($f.name)"; sha256 = $f.sha256.ToUpperInvariant(); size = $f.size } })
$fwFiles += [ordered]@{ name = $sources.firmware.licence.name; path = $sources.firmware.licence.path; sha256 = $sources.firmware.licence.sha256; size = $sources.firmware.licence.size }
$firmware = [ordered]@{ commit = $sources.firmware.commit; repository = $sources.firmware.repository; url_templates = @($sources.firmware.url_templates); install_dir = $sources.firmware.install_dir; files = $fwFiles }
$installText = Get-Content -LiteralPath (Join-Path $pkg 'INSTALL.md') -Raw
foreach ($f in $fwFiles) { if ($installText -notmatch [regex]::Escape($f.sha256.ToLowerInvariant())) { throw "INSTALL.md (GPU firmware) does not list $($f.name) with SHA256 $($f.sha256)" } }
if ($installText -notmatch $firmware.commit) { throw "INSTALL.md (GPU firmware) does not name linux-firmware $($firmware.commit)" }
'  firmware: {0} files at linux-firmware {1}, downloaded by the installer' -f $fwFiles.Count, $firmware.commit
# tester.1-6 shipped a cgit "Not found" page as LICENSE.amdgpu: no licence text may be a web page.
foreach ($f in Get-ChildItem -LiteralPath $lic -File) {
    if ((Get-Content -LiteralPath $f.FullName -TotalCount 3) -match '(?i)<!DOCTYPE|<html') { throw "licenses\$($f.Name) is a web page, not a licence text" }
}

Write-Host 'driver version'
# Two changes to the packaged INF, nothing else; the catalog below is made from this INF.
#  - DriverVer = <date>,$DriverVer.
#  - The Reboot directive in each install section (BD-060): Windows then installs the package without restarting a
#    GPU that is already started, and the GPU changes driver at the restart that ends the install, so DWM keeps its
#    devices for the rest of the session. The lab's deployment kits restart the device in place on purpose, so the
#    KMD's own INF stays without it.
. (Join-Path $PSScriptRoot 'installer\common.ps1')
$infPath = Join-Path $pkg 'payload\kmd\bc250kmd.inf'
$infText = [IO.File]::ReadAllText($infPath)
$infText = Add-InfRebootDirective $infText
'  Reboot directive in [{0}]' -f ((Get-InfInstallSections ($infText -split "`r?`n")) -join '], [')
# Only the 4th field is the release's: the first three name the KMD build (kmd_version).
$kmdBase = (([string]$sources.kmd_version) -split '\.')[0..2] -join '.'
if ($DriverVer -notmatch ('^' + [regex]::Escape($kmdBase) + '\.\d+$')) { throw "DriverVer $DriverVer does not belong to KMD $($sources.kmd_version)" }
if ($Version -notlike "$DriverVer-*") { throw "release version $Version does not start with DriverVer $DriverVer" }
$rx = [regex]'(?m)^(DriverVer\s*=\s*[\d/]+,)(\d+\.\d+\.\d+\.\d+)(\s*)$'
if ($rx.Matches($infText).Count -ne 1) { throw 'bc250kmd.inf: expected exactly one DriverVer line' }
$infText = $rx.Replace($infText, { param($m) $m.Groups[1].Value + $DriverVer + $m.Groups[3].Value })
[IO.File]::WriteAllText($infPath, $infText)
if ($infText -notmatch ('(?m)^DriverVer\s*=\s*[\d/]+,' + [regex]::Escape($DriverVer) + '\s*$')) { throw 'DriverVer rewrite failed' }
'  DriverVer {0}' -f $DriverVer

Write-Host 'sign'
$kmd = Join-Path $pkg 'payload\kmd'
$certDir = Join-Path $pkg 'payload\cert'
[void][IO.Directory]::CreateDirectory($certDir)
Copy-Item -LiteralPath $cer -Destination (Join-Path $certDir 'amdgpu-wddm-release.cer')
$release = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer
$pass = (Get-Content -LiteralPath $passFile -Raw).Trim()
function Invoke-Sign([string]$Path) {
    $r = Invoke-Headless -File $signtool -Arguments @('sign', '/fd', 'SHA256', '/f', $pfx, '/p', $pass, $Path) -TimeoutSeconds 120
    if ($r.code -ne 0) { throw "signtool sign $Path failed: $($r.text -replace [regex]::Escape($pass), '<redacted>')" }
}
Invoke-Sign (Join-Path $kmd 'bc250kmd.sys')
$r = Invoke-Headless -File $inf2cat -Arguments @("/driver:$kmd", '/os:10_X64', '/uselocaltime') -TimeoutSeconds 300
if ($r.code -ne 0) { throw "Inf2Cat failed: $($r.text)" }
Invoke-Sign (Join-Path $kmd 'bc250kmd.cat')
$pass = $null
foreach ($f in 'bc250kmd.sys', 'bc250kmd.cat') {
    $s = Get-AuthenticodeSignature -LiteralPath (Join-Path $kmd $f)
    if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $release.Thumbprint) { throw "$f is not signed by the release certificate $($release.Thumbprint)" }
    '  {0} signed by {1}' -f $f, $release.Thumbprint
}

Write-Host 'gates'
$forbidden = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.Extension -in '.pfx', '.pass', '.pem', '.key', '.p12', '.pvk' })
if ($forbidden.Count) { throw "private-key material in the package: $($forbidden.FullName -join ', ')" }
$blobs = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.Extension -eq '.bin' -or $_.FullName -like '*\payload\firmware\*' })
if ($blobs.Count) { throw "AMD firmware in the package (the installer downloads it): $($blobs.FullName -join ', ')" }
'  no private-key material, no firmware file'
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$r = Invoke-Headless -File $ps51 -Arguments @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'test-parse51.ps1'), '-Directory', $inst) -TimeoutSeconds 120
$r.text
if ($r.code -ne 0) { throw 'a script does not parse under Windows PowerShell 5.1' }
# The CLI answers every form the installer and the lab kits call (cli-commands.json), and it and both copies of
# bc250control.dll come from one build.
$r = Invoke-Headless -File $ps51 -Arguments @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'test-cli-commands.ps1'), '-Cli', (Join-Path $pkg 'payload\tools\bc250kmd_cli.exe'), '-Sources', (Join-Path $PSScriptRoot 'release-sources.json')) -TimeoutSeconds 60
$r.text
if ($r.code -ne 0) { throw 'bc250kmd_cli.exe lacks a form of cli-commands.json, or the CLI and the control DLL come from different builds' }
# No fix lost its comment on the way into this release (test-marker-guard.ps1). A train merge or a rebase can drop
# a fix together with the host tests that covered it, and then nothing else here notices.
$pwshExe = (Get-Process -Id $PID).Path
$r = Invoke-Headless -File $pwshExe -Arguments @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'test-marker-guard.ps1'), '-Repo', $repo, '-Out', (Join-Path $Out 'marker-guard')) -TimeoutSeconds 600
$r.text
if ($r.code -ne 0) { throw 'marker guard: a fix named in a source revision has no comment in this release (see above)' }
# The packaged INF's own settings against the defaults table this package carries (BD-091, tools\quality\inf_gates.py):
# every setting the INF writes is the released value and carries NOCLOBBER, so an install this installer does not drive
# cannot close one gate while another stays open. The packaged INF, not the repository's: the release re-stamps
# DriverVer and adds the Reboot directive above, and it is the packaged file that runs on a tester's computer.
$pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $pythonExe) { throw 'python is not on PATH: the inf-gates gate (BD-091) cannot run' }
$r = Invoke-Headless -File $pythonExe -Arguments @((Join-Path $repo 'tools\quality\inf_gates.py'), '--inf', $infPath, '--defaults', (Join-Path $PSScriptRoot 'installer\registry-defaults.json'), '--out', (Join-Path $Out 'inf-gates')) -TimeoutSeconds 120
$r.text
if ($r.code -ne 0) { throw 'the packaged bc250kmd.inf and installer\registry-defaults.json disagree about a driver setting (see above)' }
# The kernel sources of this repository compile and link with the WDK of this workspace (test-kmd-compile.ps1).
# quick.ps1 only exports the compile commands, so without this gate a C error in driver\kmd reached the release.
$r = Invoke-Headless -File $pwshExe -Arguments @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'test-kmd-compile.ps1'), '-Root', $Root, '-Out', (Join-Path $Out 'kmd-compile'), '-KitVersion', $KitVersion) -TimeoutSeconds 900
$r.text
if ($r.code -ne 0) { throw 'the kernel driver of this repository does not compile and link (see above)' }

Write-Host 'manifest'
# Where each payload directory lands on the tester PC (install.ps1 does the copying).
function Get-InstallLocation([string]$PackagePath) {
    $leaf = Split-Path $PackagePath -Leaf
    switch -Regex ($PackagePath) {
        '^payload/kmd/' { return "DriverStore (bc250kmd.inf)\$leaf" }
        '^payload/system32/' { return "%SystemRoot%\System32\$leaf" }
        '^payload/syswow64/' { return "%SystemRoot%\SysWOW64\$leaf" }
        '^payload/wow64/(\w+)/' { return "<InstallDir>\wow64\$($Matches[1])\$leaf" }
        '^payload/firmware/' { return "C:\BC250\firmware\$leaf" }
        '^payload/cert/' { return 'LocalMachine Root and TrustedPublisher' }
        '^payload/(\w+)/' { return "<InstallDir>\$($Matches[1])\$leaf" }
    }
    return $null
}
$components = @()
$extra = @($taskScripts | ForEach-Object { [pscustomobject]@{ component = 'tool'; path = "payload/tools/$_" } }) + @([pscustomobject]@{ component = 'certificate'; path = 'payload/cert/amdgpu-wddm-release.cer' })
foreach ($f in $sources.files + $extra) {
    $p = Join-Path $pkg ($f.path -replace '/', '\')
    $ver = (Get-Item -LiteralPath $p).VersionInfo.FileVersion
    $components += [ordered]@{ role = $f.component; package_path = $f.path; install_path = (Get-InstallLocation $f.path); version = $(if ($ver) { $ver.Trim() } else { $null }); sha256 = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
}
# Downloaded at install time: no package path, the same install path and SHA256 check in the control application.
foreach ($f in $fwFiles) { $components += [ordered]@{ role = 'firmware'; package_path = $null; install_path = "$($firmware.install_dir)\$($f.name)"; version = $null; sha256 = $f.sha256 } }
# The compatibility record (installer\compatibility.ps1, GUI plan C7): written from the final INF, installer and payload,
# listed in manifest.json like every other file, and verified below once the manifest exists.
. (Join-Path $PSScriptRoot 'installer\engine.ps1')
. (Join-Path $PSScriptRoot 'installer\release-witness.ps1')
. (Join-Path $PSScriptRoot 'installer\compatibility.ps1')
$compat = New-CompatibilityRecord -PackageRoot $pkg -Version $Version -Firmware $firmware -KmdBuild ([string]$sources.kmd_version) -KmdAbi ([string]$sources.kmd_abi) -DriverVer $DriverVer
if (-not $compat.no_live_rebind.reboot_directive) { throw 'compatibility record: the packaged INF lacks the Reboot directive' }
[IO.File]::WriteAllText((Join-Path $pkg $script:CompatibilityFile), ($compat | ConvertTo-Json -Depth 6))
'  compatibility record: engine {0}, Reboot directive in [{1}], {2} firmware files, settings table schema {3}' -f $compat.engine.contract, ($compat.no_live_rebind.install_sections -join '], ['), @($compat.firmware.files).Count, $compat.settings.table_schema
$files = @()
foreach ($f in Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($pkg.Length + 1) -replace '\\', '/'
    $files += [ordered]@{ path = $rel; sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash; size = $f.Length }
}
# The registry defaults: one table (installer\registry-defaults.json) that install.ps1 applies and the control
# application's reset reads from here.
$regDefaults = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installer\registry-defaults.json') -Raw | ConvertFrom-Json
foreach ($g in 'parameters', 'desktop_router', 'app_router', 'd3d12_applications') { if (-not $regDefaults.defaults.$g) { throw "registry-defaults.json: defaults.$g missing" } }
foreach ($n in 'EnableGpuPresentBlit', 'EnableCddDwmInterop', 'DpmMode', 'DpmMaxMHz') { if ($null -eq $regDefaults.defaults.parameters.$n) { throw "registry-defaults.json: parameters.$n missing" } }
if ($null -eq $regDefaults.defaults.desktop_router.DwmForceCpu) { throw 'registry-defaults.json: desktop_router.DwmForceCpu missing' }
if (-not $regDefaults.legacy_applied) { throw 'registry-defaults.json: legacy_applied missing' }
'  registry defaults: {0} parameters, DwmForceCpu {1}' -f @($regDefaults.defaults.parameters.PSObject.Properties).Count, $regDefaults.defaults.desktop_router.DwmForceCpu
# The H.264 encoder MFT switch (M15.11): manifest.json carries it, so the package itself says whether the installer
# registers the transform (installer\mft-h264.ps1, driver/umd/mft-h264/INSTALL.md). The identifiers must be the ones
# the installer and the DLL use; a release that registers the encoder has to carry its DLL.
. (Join-Path $PSScriptRoot 'installer\mft-h264.ps1')
$mftRelease = $null
if ($sources.PSObject.Properties['mft_h264']) {
    $mftRelease = [ordered]@{ register = [bool]$sources.mft_h264.register; package_path = [string]$sources.mft_h264.package_path
        clsid = [string]$sources.mft_h264.clsid; friendly_name = [string]$sources.mft_h264.friendly_name }
    if ($mftRelease.clsid -ne $script:MftClsid) { throw "release-sources.json mft_h264.clsid $($mftRelease.clsid) is not the installer's $($script:MftClsid)" }
    if ($mftRelease.friendly_name -ne $script:MftFriendlyName) { throw "release-sources.json mft_h264.friendly_name '$($mftRelease.friendly_name)' is not the installer's '$($script:MftFriendlyName)'" }
    if ($mftRelease.package_path -ne $script:MftPackagePath) { throw "release-sources.json mft_h264.package_path $($mftRelease.package_path) is not the installer's $($script:MftPackagePath)" }
    if ($mftRelease.register -and -not (Test-Path -LiteralPath (Join-Path $pkg ($mftRelease.package_path -replace '/', '\')))) {
        throw "release-sources.json registers the H.264 encoder MFT, but the package has no $($mftRelease.package_path): add its files row"
    }
    '  H.264 encoder MFT: {0} ({1} {2})' -f $(if ($mftRelease.register) { 'registered by the installer' } else { 'not registered by this release' }), $mftRelease.package_path, $mftRelease.clsid
}
$manifest = [ordered]@{
    schema = 1
    name = $name
    release = $Version
    version = $Version
    kmd_version = $DriverVer
    kmd_build = $sources.kmd_version
    kmd_abi = $sources.kmd_abi             # BC250_KMD_VERSION, what bc250kmd_cli info reports (start-confirm, verify)
    built_utc = [DateTime]::UtcNow.ToString('o')
    # Which of the three the work-ledger gate did: enforced (clean), debt-accepted, or absent.
    # The package states it itself, so a build that waved the gate cannot look like a gated one.
    work_ledger_gate = $script:ledgerGate
    work_ledger_gate_reason = $script:ledgerGateReason
    release_certificate = $release.Thumbprint
    control_app_exe = $sources.control_app_exe
    mft_h264 = $mftRelease        # the H.264 encoder MFT switch; absent in a release that has no encoder at all
    sources = $sources.sources
    firmware = $firmware
    defaults = $regDefaults.defaults
    licenses = @($sources.licenses | ForEach-Object { [ordered]@{ file = "licenses/$($_.file)"; repository = $_.repository; path = $_.path; ref = $_.ref; sha256 = $_.sha256 } })
    components = $components
    files = $files
}
[IO.File]::WriteAllText((Join-Path $pkg 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8))
$cv = Test-CompatibilityRecord -PackageRoot $pkg
if (-not $cv.ok) { throw "compatibility record does not verify: $($cv.detail)" }
'  compatibility record verifies against the package'
Compress-Archive -Path $pkg -DestinationPath $zip
'{0} files, package {1}' -f $files.Count, $pkg
'zip {0} SHA256 {1}' -f $zip, (Get-FileHash -LiteralPath $zip).Hash
