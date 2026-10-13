# Builds the tester release package: one folder and its zip that a tester unpacks and runs (the setup window
# setup.exe, or files\install.cmd on the command line).
#
# Layout of the unpacked release (owner, 2026-10-11): the top folder holds setup.exe and nothing else but one folder,
# files\, which holds the whole package (manifest.json, install.cmd and the other command-line entries, installer\,
# payload\, licenses\, the documents). files\ is therefore the package root that manifest.json describes and that
# every installer script works from, and setup.exe is the only file outside that list: manifest.json records it under
# "setup", so verify covers it too.
#
#   pwsh -File tools\release\build-release.ps1 -SetupApp <dir> [-Root <BC250_ROOT>] [-Out <dir>] [-Version <v>]
#
# -SetupApp is required from 0.7.216.100-tester.28 on: the output folder of tools\win\amdgpu_wddm_setup\build.ps1,
# whose own gates (unit tests, compile, fake-engine and render smokes) must have passed.
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
# ledger; manifest.json records which of the three happened), every source hash, the provenance of every payload file
# (provenance.py: a published commit and a recipe, or a download record; -RebuildCheck also rebuilds the files with
# rebuild-check.ps1 and compares the bytes; -NoProvenance -NoProvenanceReason builds a lab candidate from a work tree
# whose commits are not published yet, and manifest.json records that), signer of .sys and .cat = the release certificate, no private-key material in the
# package, the OpenGL ICD payload (BD-076: every architecture of opengl_icd.architectures is carried, and each of its
# files is an image of the right machine that exports every Drv entry point), every installer script parses under
# Windows PowerShell 5.1, every form of cli-commands.json answered by the
# packaged CLI, the guide character's art in the packaged control application (nine expressions at both sizes) and in
# the setup window (the expressions its own screens reach), under the names of the repository's own
# map, the marker guard (no fix lost its comment on the way into this release: test-marker-guard.ps1) and the
# kernel compile (driver\kmd and driver\shim build with the WDK of this workspace: test-kmd-compile.ps1). Nothing here
# opens a window: child processes run with CreateNoWindow and redirected output.
[CmdletBinding()]
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [string]$Out,
    [string]$DriverVer = '',                   # optional assertion of the mapped Windows INF quad; derived from Version
    [string]$Version = '0.7.216.100-tester.21',
    [string]$KitVersion = '10.0.26100.0',
    [string]$FirmwareDir,                        # exact pinned inputs for Inf2Cat, never copied into the public ZIP
    [string]$SetupApp,                           # required: the built setup window (build.ps1 output), copied to the top folder as setup.exe
    [switch]$AllowLedgerDebt,                    # build although finished work for a release component is unlanded
    [string]$LedgerDebtReason,                   # required with -AllowLedgerDebt: recorded in the build log and manifest
    [switch]$NoLedger,                           # build in a workspace that has no work ledger at all
    [string]$NoLedgerReason,                     # required with -NoLedger: recorded in the build log and manifest
    [switch]$NoProvenance,                       # lab candidate: build although the payload commits are not published yet
    [string]$NoProvenanceReason,                 # required with -NoProvenance: recorded in the build log and manifest
    [switch]$RebuildCheck                        # also rebuild every payload file that claims a bit-identical rebuild (slow)
)
# Release and provenance rebuilds always execute the gates, including nested analysis.
$env:BC250_GATE_NOCACHE = '1'
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
# The setup window is the default entry of an ordinary user, so a package without it is not a release.
if (-not $SetupApp) { throw '-SetupApp is required: build the setup window with tools\win\amdgpu_wddm_setup\build.ps1 -Out <dir> and pass that folder' }
if (-not (Test-Path -LiteralPath (Join-Path $SetupApp 'amdgpu_wddm_setup.exe'))) { throw "-SetupApp ${SetupApp}: no amdgpu_wddm_setup.exe" }
if (-not $Out) { $Out = Join-Path $Root 'scratch\release\out' }
$repo = Split-Path (Split-Path $PSScriptRoot)
$name = "amdgpu-wddm-tester-$Version"
$pkgRoot = Join-Path $Out $name      # what the zip unpacks to: setup.exe and files\
$pkg = Join-Path $pkgRoot 'files'    # the package root: manifest.json and everything the installer reads
$zip = "$pkgRoot.zip"
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

if (Test-Path -LiteralPath $pkgRoot) { Remove-Item -LiteralPath $pkgRoot -Recurse -Force }
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
# Provenance gate (provenance.py): every payload file names the published commit and the recipe it was built from,
# or the download it came from, and the gate checks it offline. -RebuildCheck also builds those files again from
# their commits (rebuild-check.ps1) and compares the bytes; that takes hours with the Mesa builds, so it is off by
# default.
Write-Host 'provenance'
$pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $pythonExe) { throw 'python is not on PATH: the provenance gate cannot run' }
$provenanceArgs = @((Join-Path $PSScriptRoot 'provenance.py'), 'check', '--sources', (Join-Path $PSScriptRoot 'release-sources.json'), '--root', $Root, '--repo', $repo)
# A lab candidate is built from a work tree on purpose: the lab test comes before the commit (owner rule), so the
# commit that the payload rows would name does not exist yet and no remote-tracking branch can contain it. The gate
# is then off for this build, the reason is required, and manifest.json carries both, so a package that nobody could
# tie to published commits cannot look like a gated release. A shipping package is built again after the commit,
# with the gate on.
$script:provenanceGate = 'enforced'
$script:provenanceGateReason = ''
if ($NoProvenance) {
    if (-not $NoProvenanceReason) { throw '-NoProvenance needs -NoProvenanceReason "<why this build cannot name published commits>"' }
    $script:provenanceGate = 'skipped'
    $script:provenanceGateReason = $NoProvenanceReason
    Write-Host 'provenance: GATE OFF for this build by -NoProvenance. This package is a candidate, not a release.'
    Write-Host "provenance: reason: $NoProvenanceReason"
} else {
    $r = Invoke-Headless -File $pythonExe -Arguments $provenanceArgs -TimeoutSeconds 300
    $r.text
    if ($r.code -ne 0) { throw 'provenance: a payload file has no published commit, recipe or download record (see above)' }
}
if ($RebuildCheck) {
    $pwshExe = (Get-Process -Id $PID).Path
    $r = Invoke-Headless -File $pwshExe -Arguments @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'rebuild-check.ps1'), '-Root', $Root, '-Report', (Join-Path $Out 'rebuild-check.json')) -TimeoutSeconds 21600
    $r.text
    if ($r.code -ne 0) { throw 'rebuild check: a payload file that claims a bit-identical rebuild did not rebuild to its bytes (see above)' }
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
# The logon tasks' scripts: the confirmation also writes the running-release witness (release-witness.ps1), and
# start-window.ps1 is the window task's own script (BD-080), which the logged-on user runs with their own rights.
$taskScripts = @('start-confirm.ps1', 'start-confirm-core.ps1', 'start-window.ps1', 'dwm-session.ps1', 'release-witness.ps1', 'version-identity.ps1')
$inst = Join-Path $pkg 'installer'
[void][IO.Directory]::CreateDirectory($inst)
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'installer') -File) {
    if ($f.Extension -eq '.cmd') { Copy-Item -LiteralPath $f.FullName -Destination $pkg } else { Copy-Item -LiteralPath $f.FullName -Destination $inst }
}
# The installed copy of the start-confirm task runs from <install root>\tools next to bc250kmd_cli.exe.
foreach ($f in $taskScripts) { Copy-Item -LiteralPath (Join-Path $inst $f) -Destination (Join-Path $pkg "payload\tools\$f") }
# Source-owned scripts accompany every package, including installations without the control app.
. (Join-Path $PSScriptRoot 'installer\system-tuning.ps1')
. (Join-Path $PSScriptRoot 'system-tuning-manifest.ps1')
$tuningManifest = New-SystemTuningManifest -CoreModule (Join-Path $repo 'tools\win\system-tuning\SystemTuning.Core.psm1')
$tuningRows = @()
$tuningDestinations = @('payload/system-tuning')
if (Test-Path -LiteralPath (Join-Path $pkg 'payload\control')) { $tuningDestinations += 'payload/control/system-tuning' }
foreach ($destination in $tuningDestinations) {
    [void][IO.Directory]::CreateDirectory((Join-Path $pkg $destination))
    foreach ($tuningFile in $script:SystemTuningFiles) {
        Copy-Item -LiteralPath (Join-Path $repo "tools\win\system-tuning\$tuningFile") -Destination (Join-Path $pkg "$destination/$tuningFile")
        $tuningRows += [pscustomobject]@{ component = 'system-tuning'; path = "$destination/$tuningFile" }
    }
}
# The Nagi wallpaper (owner, 2026-10-13): the obfuscated container next to the control application, which decodes
# it for each person (tools\win\amdgpu_wddm_control\art\wallpaper\PROVENANCE.md). No plain picture enters the
# package; the header and the SHA-256 of the picture inside are checked here, so a damaged file stops the build.
$wallpaperRows = @()
if (Test-Path -LiteralPath (Join-Path $pkg 'payload\control')) {
    $wpFile = Join-Path $repo 'tools\win\amdgpu_wddm_control\art\wallpaper\nagi-wallpaper.nwp'
    $wpBytes = [IO.File]::ReadAllBytes($wpFile)
    if ($wpBytes.Length -lt 44 -or [Text.Encoding]::ASCII.GetString($wpBytes, 0, 8) -ne 'NAGIWP01' -or [BitConverter]::ToUInt32($wpBytes, 40) -ne ($wpBytes.Length - 44)) { throw "$wpFile is not a whole Nagi wallpaper file" }
    $wpSha = [Security.Cryptography.SHA256]::Create()
    try {
        $wpKey = $wpSha.ComputeHash([Text.Encoding]::ASCII.GetBytes("Nagi's AMD Graphics Infrastructure"))
        $wpPlain = New-Object byte[] ($wpBytes.Length - 44)
        for ($wpAt = 0; $wpAt -lt $wpPlain.Length; $wpAt++) { $wpPlain[$wpAt] = $wpBytes[44 + $wpAt] -bxor $wpKey[$wpAt % 32] }
        if ([BitConverter]::ToString($wpSha.ComputeHash($wpPlain)) -ne [BitConverter]::ToString($wpBytes, 8, 32)) { throw "$wpFile`: the picture inside does not match its SHA-256" }
    } finally { $wpSha.Dispose() }
    Copy-Item -LiteralPath $wpFile -Destination (Join-Path $pkg 'payload\control\nagi-wallpaper.nwp')
    $wallpaperRows += [pscustomobject]@{ component = 'control'; path = 'payload/control/nagi-wallpaper.nwp' }
    '  wallpaper: payload\control\nagi-wallpaper.nwp, {0} bytes, the picture inside checked' -f $wpBytes.Length
}
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
# The notes ship as RELEASE-NOTES.md and the setup window shows them, so they may not send a tester to a path this
# package does not have. The unpacked release holds setup.exe in its top folder and everything else in files\
# (owner, 2026-10-11), so the setup path of the notes up to 0.7.216.100-tester.28 is gone.
if ($notesText -match 'setup\\amdgpu_wddm_setup\.exe') {
    throw "release notes $notes still name setup\amdgpu_wddm_setup.exe: a tester double-clicks setup.exe in the unpacked folder, and the command line is files\install.cmd"
}
Copy-Item -LiteralPath $notes -Destination (Join-Path $pkg 'RELEASE-NOTES.md')
foreach ($t in Get-ChildItem -LiteralPath $notesDir -File -Filter "$Version.*.md") {
    $lang = $t.Name.Substring($Version.Length + 1) -replace '\.md$', ''
    if ($lang -notmatch '^[a-z]{2}$') { throw "release notes translation $($t.Name): expected <version>.<two-letter language>.md" }
    Copy-Item -LiteralPath $t.FullName -Destination (Join-Path $pkg "RELEASE-NOTES.$lang.md")
}
'  release notes: RELEASE-NOTES.md{0}' -f $(if (@(Get-ChildItem -LiteralPath $pkg -Filter 'RELEASE-NOTES.*.md').Count) { ' + ' + ((Get-ChildItem -LiteralPath $pkg -Filter 'RELEASE-NOTES.*.md' | ForEach-Object { $_.Name }) -join ', ') } else { '' })
# docs\testing\release-notes\pending holds tester-facing lines written before the release that carries them. Nothing
# reads that folder, so a note left there ships nowhere: the file 0.7.208-log-lines.md sat through tester.24 to
# tester.28 unread. Name every pending file in the build log, so the release step either folds the lines into
# <version>.md and deletes the file, or decides out loud to leave it for the next release.
$pendingDir = Join-Path $notesDir 'pending'
if (Test-Path -LiteralPath $pendingDir) {
    foreach ($p in Get-ChildItem -LiteralPath $pendingDir -File -Filter '*.md') {
        Write-Warning ("pending release-notes lines not folded into ${Version}.md: docs\testing\release-notes\pending\{0}" -f $p.Name)
    }
}
# The setup window (required, checked at the start): the one file in the unpacked release's top folder, named
# setup.exe because that is what a tester double-clicks. The installer's continuation runs it from the staged closure
# after each restart that a setup-window run asked for; its copy there is next to the manifest (common.ps1).
$setupSrc = Join-Path $SetupApp 'amdgpu_wddm_setup.exe'
$setupPkg = Join-Path $pkgRoot 'setup.exe'
Copy-Item -LiteralPath $setupSrc -Destination $setupPkg
# A .NET application configuration file, when the build makes one, belongs to the exe under its new name.
foreach ($f in Get-ChildItem -LiteralPath $SetupApp -File -Filter 'amdgpu_wddm_setup.exe.config') { Copy-Item -LiteralPath $f.FullName -Destination "$setupPkg.config" }
$setupHash = (Get-FileHash -LiteralPath $setupPkg -Algorithm SHA256).Hash
'  setup window: {0} -> {1} ({2})' -f $setupSrc, $setupPkg, $setupHash.Substring(0, 8)
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
# an earlier release where a tester has to know about it. A version token is read whole, with the "vR" of a second
# issue (0.7.216.100-tester.29v2, owner 2026-10-13): read without it, every mention of this package's own version
# would count as the other release 0.7.216.100-tester.29.
foreach ($d in Get-ChildItem -LiteralPath $pkg -File -Filter '*.md') {
    if ($d.Name -like 'RELEASE-NOTES*') { continue }
    $versionNames = @([regex]::Matches((Get-Content -LiteralPath $d.FullName -Raw), '\d+\.\d+\.\d+\.\d+-tester\.\d+(?:v\d+)?') | ForEach-Object { $_.Value } | Sort-Object -Unique | Where-Object { $_ -ne $Version })
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
$firmware = [ordered]@{ delivery = 'driver-store-v1'; commit = $sources.firmware.commit; repository = $sources.firmware.repository; url_templates = @($sources.firmware.url_templates); install_dir = '%13%'; files = $fwFiles }
. (Join-Path $PSScriptRoot 'installer\firmware-package.ps1')
if (-not $FirmwareDir) { $FirmwareDir = Join-Path $Root 'ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu' }
Copy-DriverStoreFirmware -Firmware $firmware -FromDir $FirmwareDir -Destination (Join-Path $pkg 'payload\kmd')
$installText = Get-Content -LiteralPath (Join-Path $pkg 'INSTALL.md') -Raw
foreach ($f in $fwFiles) { if ($installText -notmatch [regex]::Escape($f.sha256.ToLowerInvariant())) { throw "INSTALL.md (GPU firmware) does not list $($f.name) with SHA256 $($f.sha256)" } }
if ($installText -notmatch $firmware.commit) { throw "INSTALL.md (GPU firmware) does not name linux-firmware $($firmware.commit)" }
'  firmware: {0} files at linux-firmware {1}, downloaded by the installer' -f $fwFiles.Count, $firmware.commit
# tester.1-6 shipped a cgit "Not found" page as LICENSE.amdgpu: no licence text may be a web page.
foreach ($f in Get-ChildItem -LiteralPath $lic -File) {
    if ((Get-Content -LiteralPath $f.FullName -TotalCount 3) -match '(?i)<!DOCTYPE|<html') { throw "licenses\$($f.Name) is a web page, not a licence text" }
}

# The guide character has to be in the shipped programs. Every package up to 0.7.216.100-tester.21 was built by a
# recipe that passed no art directory at all, so the guide panel was the text-only panel on every install: the
# expressions are named in the exe only when the art was embedded. The names come from the repository's own map, so
# a renamed expression is a finding here and not a silent text-only panel on a tester's PC.
$guideMap = [regex]::Matches((Get-Content -LiteralPath (Join-Path $repo 'tools\win\amdgpu_wddm_control\src\Guide.cs') -Raw),
    '\{ "(?<expr>\d\d-[a-z-]+)", "(?<file>[a-z-]+)" \}')
if ($guideMap.Count -ne 9) { throw "Guide.ArtFile: $($guideMap.Count) expressions found, 9 expected" }
$guideResources = @(foreach ($m in $guideMap) { foreach ($size in 128, 256) { "nagi.$($m.Groups['expr'].Value)@$size.png" } })
# The setup window embeds only the expressions its own causes reach (SetupGuide.ArtExpressions), so this gate asks it
# for the expression ids its source names and for nothing more. A new expression there is covered by that alone.
$setupExpressions = @([regex]::Matches((Get-Content -LiteralPath (Join-Path $repo 'tools\win\amdgpu_wddm_setup\src\EngineModel.cs') -Raw),
        '"(?<expr>\d\d-[a-z-]+)"') | ForEach-Object { $_.Groups['expr'].Value } | Sort-Object -Unique)
if (-not $setupExpressions.Count) { throw 'SetupGuide: no guide expression found in EngineModel.cs' }
$unknown = @($setupExpressions | Where-Object { $e = $_; -not ($guideMap | Where-Object { $_.Groups['expr'].Value -eq $e }) })
if ($unknown.Count) { throw "SetupGuide names the expression $($unknown -join ', '), which Guide.ArtFile does not have" }
# The setup window embeds the @256 picture alone, by its own contract: its smallest slot is 176 device pixels at 96
# DPI, larger than the @128 picture, so a @128 resource could never be drawn (EngineModel.cs, SetupGuide.ArtPixels,
# and the -Sizes 256 of tools\win\amdgpu_wddm_setup\build.ps1). Asking for both sizes here refused every setup
# window ever built, this release's included. The control application above keeps both sizes: it draws the small one.
$setupResources = @(foreach ($e in $setupExpressions) { "nagi.$e@256.png" })
# The control application is a component of every package: its path comes from release-sources.json, so a renamed
# payload path cannot make this gate match nothing. The setup window is required too (-SetupApp, checked at the start).
$controlFiles = @($sources.files | Where-Object { $_.component -eq 'control' -and $_.path -like '*amdgpu_wddm_control.exe' })
if ($controlFiles.Count -ne 1) { throw "release-sources.json: $($controlFiles.Count) control-application exe entries, 1 expected" }
$checkedPrograms = 0
foreach ($program in @(
        @{ name = 'control application'; path = (Join-Path $pkg ($controlFiles[0].path -replace '/', '\')); want = $guideResources; required = $true },
        @{ name = 'setup window'; path = $setupPkg; want = $setupResources; required = $true })) {
    if (-not (Test-Path -LiteralPath $program.path)) {
        if ($program.required) { throw "guide art: the package has no $($program.name) at $($program.path)" }
        continue
    }
    $exeText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($program.path))
    $missingArt = @($program.want | Where-Object { -not $exeText.Contains($_) })
    if ($missingArt.Count) {
        throw ("guide art: the packaged $($program.name) has no resource $($missingArt -join ', '). " +
               'Build it with tools\win\amdgpu_wddm_control\build.ps1 (the art of tools\win\amdgpu_wddm_control\art\nagi is embedded by default).')
    }
    $checkedPrograms++
    '  guide art: {0} resources in the {1}' -f @($program.want).Count, $program.name
}
if ($checkedPrograms -eq 0) { throw 'guide art: no packaged program was checked' }

Write-Host 'driver version'
# Stamp DriverVer and our product identity. The display class forbids Reboot (BD-152).
# The installer defers binding through DiInstallDriverW(INSTALL_AS_SET), preserving BD-060 outside the INF.
. (Join-Path $PSScriptRoot 'installer\common.ps1')
. (Join-Path $PSScriptRoot 'installer\deferred-driver.ps1')
$infPath = Join-Path $pkg 'payload\kmd\bc250kmd.inf'
$infText = [IO.File]::ReadAllText($infPath)
if (Test-InfHasReboot ($infText -split "`r?`n")) { throw 'display INF must not contain Reboot (BD-152)' }
# BD-152: package identity is installation-owned. Never derive it from the embedded KMD build.
$productQuad = Get-ProductPackageQuad $Version
$mappedQuad = Get-PackageDriverQuad $productQuad
if ($DriverVer -and $DriverVer -cne $mappedQuad) { throw "DriverVer $DriverVer differs from package mapping $mappedQuad" }
$DriverVer = $mappedQuad
# BD-150: the date is the release's too (release-sources.json driver_date), and the release must rank above the one it
# supersedes. PnP orders packages of one rank by DriverVer date, then by version, and keeps the installed package on a
# tie: tester.29 and 29v2 both said 10/12/2026,0.7.216.100, and the 29v2 install left the tester.29 KMD on the GPU. The
# date is pinned in the sources, not taken from the clock, so a rebuild gives the same INF.
$rx = [regex]'(?m)^(DriverVer\s*=\s*)(\d\d/\d\d/\d{4}),(\d+\.\d+\.\d+\.\d+)(\s*)$'
if ($rx.Matches($infText).Count -ne 1) { throw 'bc250kmd.inf: expected exactly one DriverVer line' }
function ConvertFrom-InfDate([string]$Text, [string]$What) {
    $d = [DateTime]::MinValue
    if (-not [DateTime]::TryParseExact($Text, 'MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$d)) { throw "${What}: '$Text' is not an INF date (MM/dd/yyyy)" }
    return $d
}
$kmdDate = ConvertFrom-InfDate $rx.Match($infText).Groups[2].Value 'the KMD INF DriverVer date'
if (-not $sources.driver_date) { throw 'release-sources.json: no driver_date (the DriverVer date of this release, MM/dd/yyyy)' }
$driverDate = ConvertFrom-InfDate ([string]$sources.driver_date) 'release-sources.json driver_date'
if ($driverDate -lt $kmdDate) { throw "driver_date $($sources.driver_date) is older than the KMD INF date $($kmdDate.ToString('MM/dd/yyyy'))" }
# Inf2Cat refuses a DriverVer date after the build computer's local date (22.9.7, postdated DriverVer), so a date
# that is tomorrow in local time fails at the catalog step after every other gate has run. Refuse it here.
if ($driverDate -gt [DateTime]::Now.Date) { throw "driver_date $($sources.driver_date) is after today ($([DateTime]::Now.ToString('MM/dd/yyyy'))): Inf2Cat refuses a postdated DriverVer" }
if (-not $sources.supersedes -or -not $sources.supersedes.release -or -not $sources.supersedes.driver_ver) {
    throw 'release-sources.json: no supersedes {release, driver_ver}: the release this one replaces and its DriverVer line value'
}
if ([string]$sources.supersedes.driver_ver -notmatch '^(\d\d/\d\d/\d{4}),(\d+\.\d+\.\d+\.\d+)$') { throw "supersedes.driver_ver '$($sources.supersedes.driver_ver)' is not <MM/dd/yyyy>,<a.b.c.d>" }
$oldDate = ConvertFrom-InfDate $Matches[1] 'supersedes.driver_ver'
$oldVer = [version]$Matches[2]
if (-not (($driverDate -gt $oldDate) -or (($driverDate -eq $oldDate) -and ([version]$DriverVer -gt $oldVer)))) {
    throw ("DriverVer $($sources.driver_date),$DriverVer does not rank above $($sources.supersedes.release) ($($sources.supersedes.driver_ver)): " +
           'Windows would keep that package. Set a later driver_date, or a higher product package version on the same date.')
}
$driverDateText = $driverDate.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
$infText = $rx.Replace($infText, { param($m) $m.Groups[1].Value + $driverDateText + ',' + $DriverVer + $m.Groups[4].Value })
$productRx = [regex]'(?m)^(HKR,, ProductPackageVersion, 0x00000000, ")[0-9.]+("\r?)$'
if ($productRx.Matches($infText).Count -ne 1) { throw 'INF must name ProductPackageVersion once' }
$infText = $productRx.Replace($infText, { param($m) $m.Groups[1].Value + $productQuad + $m.Groups[2].Value })
[IO.File]::WriteAllText($infPath, $infText)
if ($infText -notmatch ('(?m)^DriverVer\s*=\s*' + [regex]::Escape("$driverDateText,$DriverVer") + '\s*$')) { throw 'DriverVer rewrite failed' }
'  DriverVer {0},{1} (above {2}, {3})' -f $driverDateText, $DriverVer, $sources.supersedes.release, $sources.supersedes.driver_ver

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
# BD-150: Windows names a driver package by its INF, so two packages with different bc250kmd.sys files must never share
# one INF: pnputil answers "Already exists in the system" for the second and keeps the first one's KMD. One comment line
# under DriverVer names the release and the signed KMD, so the INF changes whenever the KMD does.
$kmdSha = (Get-FileHash -LiteralPath (Join-Path $kmd 'bc250kmd.sys') -Algorithm SHA256).Hash
$infText = [IO.File]::ReadAllText($infPath)
$dvLine = [regex]'(?m)^DriverVer\s*=.*$'
if ($dvLine.Matches($infText).Count -ne 1) { throw 'bc250kmd.inf: expected exactly one DriverVer line' }
$identity = "; release $Version, bc250kmd.sys SHA256 $kmdSha"
$infText = $dvLine.Replace($infText, { param($m) $cr = $(if ($m.Value.EndsWith("`r")) { "`r" } else { '' }); $m.Value.TrimEnd("`r") + $cr + "`n" + $identity + $cr })
[IO.File]::WriteAllText($infPath, $infText)
'  INF identity: {0}' -f $identity
$r = Invoke-Headless -File $inf2cat -Arguments @("/driver:$kmd", '/os:10_X64', '/uselocaltime') -TimeoutSeconds 300
if ($r.code -ne 0) { throw "Inf2Cat failed: $($r.text)" }
Invoke-Sign (Join-Path $kmd 'bc250kmd.cat')
$pass = $null
foreach ($f in 'bc250kmd.sys', 'bc250kmd.cat') {
    $s = Get-AuthenticodeSignature -LiteralPath (Join-Path $kmd $f)
    if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $release.Thumbprint) { throw "$f is not signed by the release certificate $($release.Thumbprint)" }
    '  {0} signed by {1}' -f $f, $release.Thumbprint
}
# The catalog was built from a complete package. Check its actual members before
# excluding the eight inputs from the public ZIP; hydration must reproduce them.
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$r = Invoke-Headless -File $ps51 -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'test-firmware-catalog.ps1'), '-Directory', $kmd) -TimeoutSeconds 60
if ($r.code -ne 0) { throw "firmware catalog membership: $($r.text)" }
foreach ($fwBlobName in Get-DriverStoreFirmwareNames) { Remove-Item -LiteralPath (Join-Path $kmd $fwBlobName) -Force }

Write-Host 'gates'
$forbidden = @(Get-ChildItem -LiteralPath $pkgRoot -Recurse -File | Where-Object { $_.Extension -in '.pfx', '.pass', '.pem', '.key', '.p12', '.pvk' })
if ($forbidden.Count) { throw "private-key material in the package: $($forbidden.FullName -join ', ')" }
$blobs = @(Get-ChildItem -LiteralPath $pkgRoot -Recurse -File | Where-Object { $_.Extension -eq '.bin' -or $_.FullName -like '*\payload\firmware\*' })
if ($blobs.Count) { throw "AMD firmware in the package (the installer downloads it): $($blobs.FullName -join ', ')" }
'  no private-key material, no firmware file'
# The unpacked release's top folder: setup.exe (with its configuration file, when there is one) and the one folder
# files\, nothing else (owner, 2026-10-11). A document or a script that creeps back into the top folder is a finding
# here, not something a tester has to sort out.
$topFiles = @(Get-ChildItem -LiteralPath $pkgRoot -File | ForEach-Object { $_.Name } | Where-Object { $_ -notin 'setup.exe', 'setup.exe.config' })
$topDirs = @(Get-ChildItem -LiteralPath $pkgRoot -Directory | ForEach-Object { $_.Name } | Where-Object { $_ -ne 'files' })
if ($topFiles.Count -or $topDirs.Count) { throw "the unpacked package's top folder may hold only setup.exe and files\: it also holds $(@($topFiles + $topDirs) -join ', ')" }
'  top folder: setup.exe and files\ only'
# The packaged copies against release-sources.json: each is its source byte for byte, the re-signed kernel driver keeps
# its recorded Authenticode digest, and only an entry that names its "release_edits" (the INF) differs otherwise.
if ($NoProvenance) {
    '  package provenance: GATE OFF by -NoProvenance (the commits of this candidate are not published yet)'
} else {
    $r = Invoke-Headless -File $pythonExe -Arguments ($provenanceArgs + @('--package', $pkg)) -TimeoutSeconds 300
    $r.text -split "`r?`n" | Where-Object { $_ -match '^(FAIL|provenance: (FAIL|PASS))' } | ForEach-Object { "  package $_" }
    if ($r.code -ne 0) { throw 'provenance: a packaged file is not the recorded one (see above)' }
}
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
# DriverVer, our product identity and the image comment above; the packaged file runs on a tester's computer.
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

# Full owns the controls removed from the incremental KMD profile. No cache can
# supply a PASS here; the receipt identifies every executed check and its deadline.
$qualityCmd = Join-Path $repo 'tools\quality\quick.cmd'
$qualityOut = Join-Path $Out 'quality-full'
& $qualityCmd $Root $qualityOut $repo 'Full' (Join-Path $Root 'toolchain\nuget')
if ($LASTEXITCODE -ne 0) { throw 'Full uncached release quality gates failed' }

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
        # The Microsoft Visual C++ runtime installer runs from the package and is never copied anywhere: the files it
        # installs are Microsoft's own, in System32, under Microsoft's version rules. install.ps1 copies the payload
        # directories it names in $dirs, and redist is not one of them.
        '^payload/redist/' { return 'run from the package (Microsoft installs its own files in System32)' }
        '^payload/(.+)$' { return ('<InstallDir>\' + $Matches[1].Replace('/', '\')) }
    }
    return $null
}
$components = @()
$extra = @($taskScripts | ForEach-Object { [pscustomobject]@{ component = 'tool'; path = "payload/tools/$_" } }) + @([pscustomobject]@{ component = 'certificate'; path = 'payload/cert/amdgpu-wddm-release.cer' }) + $tuningRows + $wallpaperRows
foreach ($f in $sources.files + $extra) {
    $p = Join-Path $pkg ($f.path -replace '/', '\')
    $ver = (Get-Item -LiteralPath $p).VersionInfo.FileVersion
    $components += [ordered]@{ role = $f.component; package_path = $f.path; install_path = (Get-InstallLocation $f.path); version = $(if ($ver) { $ver.Trim() } else { $null }); sha256 = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
}
# Downloaded at install time: no package path, the same install path and SHA256 check in the control application.
foreach ($f in $fwFiles) {
    $fwInstalled = if ($f.name -ceq 'LICENSE.amdgpu') { '<InstallDir>\licenses\LICENSE.amdgpu' }
        else { "DriverStore (bc250kmd.inf)\$($f.name)" }
    $components += [ordered]@{ role = 'firmware'; package_path = $null; install_path = $fwInstalled; version = $null; sha256 = $f.sha256 }
}
# The optional components of this release (installer\components.ps1): our llama.cpp build and our Ollama build are
# release assets, not package files. tools\release\components.json is the pinned list; this writes it into the package
# as installer\components.json with {release} expanded, and manifest.json names it under optional_components, so the
# existing package-integrity check covers it like every other file. A component whose asset SHA-256 is not filled in
# has no pin: it stays in the list, the installer refuses it with a plain message and downloads nothing. A package can
# therefore be built before its component zips are uploaded, and it will not install one unchecked.
. (Join-Path $PSScriptRoot 'installer\components.ps1')
$optionalComponents = $null
$componentSource = Join-Path $PSScriptRoot 'components.json'
if (Test-Path -LiteralPath $componentSource -PathType Leaf) {
    $cmJson = Get-Content -LiteralPath $componentSource -Raw | ConvertFrom-Json
    $cmJson.release = $Version
    if ($cmJson.PSObject.Properties['comment']) { $cmJson.PSObject.Properties.Remove('comment') }
    foreach ($c in @($cmJson.components)) {
        if ($c.PSObject.Properties['comment']) { $c.PSObject.Properties.Remove('comment') }
        $c.asset.name = ([string]$c.asset.name).Replace('{release}', $Version)
        $c.asset.url_templates = @(@($c.asset.url_templates) | ForEach-Object { ([string]$_).Replace('{release}', $Version) })
    }
    $cmProblems = @(Test-ComponentsManifest $cmJson)
    if ($cmProblems.Count) { throw "tools\release\components.json: $($cmProblems -join '; ')" }
    # An asset name that carries no {release} is the literal name of one release's asset. When it names another
    # release's train than this build, its address is a 404 at install time, and only a tester would find out. The
    # asset is downloaded by its SHA-256, so the name is the one thing nothing else checks.
    # The token is read whole, with the "vR" of a second issue (0.7.216.100-tester.29v2): read without it, the
    # warning would name this release tester.29.
    if ($Version -match 'tester\.(\d+(?:v\d+)?)') {
        $thisTrain = $Matches[1]
        foreach ($c in @($cmJson.components)) {
            $assetName = [string]$c.asset.name
            if ($assetName -match 'tester\.(\d+(?:v\d+)?)' -and $Matches[1] -ne $thisTrain) {
                "  WARNING: component '$([string]$c.id)' names the asset $assetName, which carries tester.$($Matches[1]) while this release is tester.$thisTrain. Upload the asset under that name, or write the new name, size and sha256 into tools\release\components.json."
            }
        }
    }
    [IO.File]::WriteAllText((Join-Path $pkg 'installer\components.json'), ($cmJson | ConvertTo-Json -Depth 8))
    $cmPinned = @(@($cmJson.components) | Where-Object { -not (Get-ComponentPinProblem $_) } | ForEach-Object { [string]$_.id })
    $cmUnpinned = @(@($cmJson.components) | Where-Object { Get-ComponentPinProblem $_ } | ForEach-Object { [string]$_.id })
    $optionalComponents = [ordered]@{
        file = 'installer/components.json'
        models_dir = [string]$cmJson.models_dir
        offered = @(@($cmJson.components) | ForEach-Object { [string]$_.id })
        pinned = $cmPinned
        unpinned = $cmUnpinned
        models = @(@($cmJson.models) | ForEach-Object { [string]$_.id })
    }
    '  components: {0} offered ({1}), {2} with a pinned download, {3} models offered' -f @($cmJson.components).Count, (@($optionalComponents.offered) -join ', '), $cmPinned.Count, @($cmJson.models).Count
    foreach ($id in $cmUnpinned) { "  WARNING: component '$id' has no SHA-256 in tools\release\components.json: this package will refuse it. Upload its zip as a release asset and fill size and sha256." }
} else { '  components: tools\release\components.json is not in this workspace: this package offers no optional component' }

# The compatibility record (installer\compatibility.ps1, GUI plan C7): written from the final INF, installer and payload,
# listed in manifest.json like every other file, and verified below once the manifest exists.
. (Join-Path $PSScriptRoot 'installer\engine.ps1')
. (Join-Path $PSScriptRoot 'installer\release-witness.ps1')
. (Join-Path $PSScriptRoot 'installer\compatibility.ps1')
$versionIdentity = New-PackageVersionIdentity -Release $Version -DriverDate $driverDateText -KmdBuild ([string]$sources.kmd_version) -KmdAbi ([string]$sources.kmd_abi) -KmdSha256 $kmdSha
$compat = New-CompatibilityRecord -PackageRoot $pkg -Version $Version -Firmware $firmware -KmdBuild ([string]$sources.kmd_version) -KmdAbi ([string]$sources.kmd_abi) -DriverVer $DriverVer -VersionIdentity $versionIdentity
if ($compat.no_live_rebind.method -ne 'di-install-as-set' -or $compat.no_live_rebind.reboot_directive) { throw 'compatibility record: deferred driver-set contract missing' }
[IO.File]::WriteAllText((Join-Path $pkg $script:CompatibilityFile), ($compat | ConvertTo-Json -Depth 6))
'  compatibility record: engine {0}, {1}, {2} firmware files, settings table schema {3}' -f $compat.engine.contract, $compat.no_live_rebind.method, @($compat.firmware.files).Count, $compat.settings.table_schema
$files = @()
foreach ($f in Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($pkg.Length + 1) -replace '\\', '/'
    $files += [ordered]@{ path = $rel; sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash; size = $f.Length }
}
# The registry defaults: one table (installer\registry-defaults.json) that install.ps1 applies and the control
# application's reset reads from here.
$regDefaults = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installer\registry-defaults.json') -Raw | ConvertFrom-Json
foreach ($g in 'parameters', 'desktop_router', 'app_router', 'd3d12_applications', 'graphics_drivers') { if (-not $regDefaults.defaults.$g) { throw "registry-defaults.json: defaults.$g missing" } }
foreach ($n in 'EnableGpuPresentBlit', 'EnableCddDwmInterop', 'DpmMode', 'DpmMaxMHz', 'CuMode') { if ($null -eq $regDefaults.defaults.parameters.$n) { throw "registry-defaults.json: parameters.$n missing" } }
# The graphics cores of an install (BD-077): the table names the number the driver starts with, and the only numbers
# the driver and the control application accept are the firmware's 24 and all 40. A package that names another number
# would install a value the driver refuses (reason INVALID_SETTING) and a number the application cannot show.
$cuDefault = [int]$regDefaults.defaults.parameters.CuMode
if ($cuDefault -ne 24 -and $cuDefault -ne 40) { throw "registry-defaults.json: parameters.CuMode is '$cuDefault', not 24 or 40" }
if ($null -eq $regDefaults.defaults.desktop_router.DwmForceCpu) { throw 'registry-defaults.json: desktop_router.DwmForceCpu missing' }
# How long Windows waits for the graphics (BD-079): inside the range the control application offers, or the app would
# show a release default it cannot choose (tools/win/amdgpu_wddm_control/src/TdrSetting.cs, Min and Max).
$tdrDefault = $regDefaults.defaults.graphics_drivers.TdrDelay
if ($null -eq $tdrDefault -or $tdrDefault -lt 2 -or $tdrDefault -gt 60) { throw "registry-defaults.json: graphics_drivers.TdrDelay is '$tdrDefault', not a number of seconds between 2 and 60" }
if (-not $regDefaults.legacy_applied) { throw 'registry-defaults.json: legacy_applied missing' }
'  registry defaults: {0} parameters, DwmForceCpu {1}, TdrDelay {2} s, CuMode {3}' -f @($regDefaults.defaults.parameters.PSObject.Properties).Count, $regDefaults.defaults.desktop_router.DwmForceCpu, $tdrDefault, $cuDefault
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
# The package names itself after its folder. A loop that took $name for its own variable once named tester.28 r1
# after the Ollama asset, and the validation suite then found no zip beside the folder.
if ($name -ne (Split-Path -Leaf $pkgRoot)) { throw "build-release: the package name is '$name', its folder is '$(Split-Path -Leaf $pkgRoot)'" }
# The OpenGL ICD (BD-076, GitHub issue #8): manifest.json carries the switch, so the package itself says whether the
# installer registers an OpenGL driver and for which architectures ("architectures"), and this gate holds the payload
# to the contract. Every architecture of that list must be in the package, as an image of the right machine that
# exports every Drv entry point the OpenGL runtime calls (installer\common.ps1 holds the list and the sources of the
# contract): a file that does not is no ICD, and a registration naming it would leave every OpenGL application of that
# architecture on the tester's computer without a driver, instead of the software renderer Windows gives it without a
# registration. A missing file is never a silent skip: the build stops here and names what is missing.
# The other way round stops the build as well, and the reason is the wow64 tree: the installer copies that tree whole,
# so a file of an architecture this release does not register would be copied into the install root and deleted again
# by the narrowing step of the same run, while manifest.json carries a component row that says it is installed. The
# package has to say which of the two it means - register that architecture, or drop its files row - and
# test-wow64.ps1 and test-opengl-icd.ps1 hold a cut package to the same rule. Both rules are
# installer\common.ps1 Get-OpenGlPayloadProblems, so the build and the gates read one rule.
$glRelease = $null
if ($sources.PSObject.Properties['opengl_icd']) {
    $gl = Get-OpenGlReleaseSwitch -Manifest $sources -PackageRoot $pkg
    $glRelease = [ordered]@{ register = [bool]$gl.register; architectures = @($gl.architectures)
        package_path = [string]$gl.package_path; package_path_wow = [string]$gl.package_path_wow
        value_type = [string]$gl.value_type; version = [int]$gl.version; flags = [int]$gl.flags }
    if ($gl.register) {
        $glPayload = @(Get-OpenGlPayloadProblems -Switch $gl)
        if ($glPayload.Count) { throw ('release-sources.json: ' + ($glPayload -join '; ')) }
        $glFiles = @(Get-OpenGlArchFiles -Switch $gl)
        $glProblems = @()
        foreach ($f in $glFiles) { $glProblems += @(Test-OpenGlIcdFile -Path $f.source -Machine $f.machine) }
        if ($glProblems.Count) { throw ('OpenGL ICD payload: ' + ($glProblems -join '; ')) }
        # The counted exports of each file, not the length of the contract list: the line says what the package holds.
        $glShown = (@($glFiles | ForEach-Object { '{0} ({1}, {2} Drv entry points)' -f $_.package_path, $_.arch, @(Get-PeExportNames $_.source | Where-Object { $_ -like 'Drv*' }).Count }) -join ' and ')
        '  OpenGL ICD: {0}, all {1} of the contract, {2}, version {3}, flags {4}' -f $glShown, @($script:OpenGlDrvExports).Count, $gl.value_type, $gl.version, $gl.flags
        if (-not $gl.register_x86) { '  OpenGL ICD: no 32-bit registration in this release; 32-bit OpenGL programs keep the renderer of Windows itself' }
    } else {
        '  OpenGL ICD: not registered by this release'
    }
}
$manifest = [ordered]@{
    schema = 1
    name = $name
    release = $Version
    version = $Version
    kmd_version = $DriverVer
    version_identity = $versionIdentity
    kmd_build = $sources.kmd_version
    kmd_abi = $sources.kmd_abi             # BC250_KMD_VERSION, what bc250kmd_cli info reports (start-confirm, verify)
    built_utc = [DateTime]::UtcNow.ToString('o')
    # Which of the three the work-ledger gate did: enforced (clean), debt-accepted, or absent.
    # The package states it itself, so a build that waved the gate cannot look like a gated one.
    work_ledger_gate = $script:ledgerGate
    work_ledger_gate_reason = $script:ledgerGateReason
    # enforced, or skipped by -NoProvenance in a lab candidate whose commits are not published yet.
    provenance_gate = $script:provenanceGate
    provenance_gate_reason = $script:provenanceGateReason
    release_certificate = $release.Thumbprint
    # The setup window: the one file of the zip that is not under the package root, because the unpacked release puts
    # it in the top folder (owner, 2026-10-11). "file" is a name, not a path: the unpacked release keeps it beside
    # files\, a kept repair set and a prepared folder keep their copy in the package root (common.ps1,
    # Get-PackageSetupExe). Test-PackageManifest hashes it wherever it is, so verify covers it like every other file.
    setup = [ordered]@{ file = 'setup.exe'; sha256 = $setupHash; size = (Get-Item -LiteralPath $setupPkg).Length }
    control_app_exe = $sources.control_app_exe
    system_tuning = $tuningManifest
    mft_h264 = $mftRelease        # the H.264 encoder MFT switch; absent in a release that has no encoder at all
    opengl_icd = $glRelease       # the OpenGL ICD switch (BD-076); absent in a release that has no GL ICD at all
    optional_components = $optionalComponents   # the extras (installer/components.json); $null in a release without any
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
Compress-Archive -Path $pkgRoot -DestinationPath $zip
'{0} files under files\ plus setup.exe, package {1}' -f $files.Count, $pkgRoot
'zip {0} SHA256 {1}' -f $zip, (Get-FileHash -LiteralPath $zip).Hash

# The single-file installer, beside the zip (owner, 2026-10-12: "dlaczego setup.exe nie zawiera zipa w sobie? to
# lepsza metoda!"): one exe that carries this whole release inside it, for a tester who would rather download one
# file than unpack an archive. The zip stays: it is the same release, and a tester who already has it changes
# nothing. make-setup-exe.ps1 -Check is the gate - it reads the exe back and compares every file of the payload with
# the file of this package, byte for byte, and -ReleaseZip compares it with the entries of the zip written above, so an exe
# whose payload is not the files of this release's archive cannot be published.
$setupOne = Join-Path $Out "amdgpu-wddm-setup-$Version.exe"
& (Join-Path $PSScriptRoot 'make-setup-exe.ps1') -PackageTop $pkgRoot -Out $setupOne -Check -ReleaseZip $zip | ForEach-Object { "  $_" }
if (-not (Test-Path -LiteralPath $setupOne -PathType Leaf)) { throw "make-setup-exe.ps1 wrote no $setupOne" }
'single exe {0} SHA256 {1}' -f $setupOne, (Get-FileHash -LiteralPath $setupOne).Hash
