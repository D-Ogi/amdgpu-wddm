# Host test of the optional components of the release (installer\components.ps1), under Windows PowerShell 5.1 like
# the installer itself (called by test-dryrun.ps1).
#   powershell -File tools\release\test-components.ps1 -Installer <package>\installer [-Source <repo>\tools\release\components.json] -WorkRoot <dir>
# Writes only under -WorkRoot and HKCU:\Software\amdgpu-wddm-installer-test-components. Nothing under HKLM, nothing in
# the real Start menu, no machine PATH, no network: $script:NetworkBlocked is on for every download case.
# Cases: the release's own components.json validates; a broken manifest is refused, one message per fault; a component
# without a SHA-256 pin is refused and nothing is downloaded; the switch combinations (-Components, -NoComponents,
# -Models, -NoModels, unattended defaults, the setup window) resolve as documented; a changed file in an offline
# folder is refused and not staged; a folder prepared with components\ and models\ side by side is read from both;
# a file that is not in the folder at all goes to the download route; a zip with an unsafe entry is refused; the model
# question says what a model costs when two extras each keep a copy; an install writes the folder, the backend
# setting, the PATH entry and the shortcuts, a dry run writes none of them; a hook is judged by its exit code, not by
# its output, so a non-English Windows changes nothing (issue #35); the uninstall removes every registration and keeps
# the models unless -RemoveModels.
param([Parameter(Mandatory)][string]$Installer, [string]$Source, [Parameter(Mandatory)][string]$WorkRoot,
      [string]$Release = '0.0.0.0-tester.0')   # used only when the installer folder has no components.json yet
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
. (Join-Path $Installer 'components.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Matched($Lines, [string]$Pattern) { return (@(@($Lines) -match $Pattern).Count -gt 0) }
function Copy-Json($O) { return ($O | ConvertTo-Json -Depth 10 | ConvertFrom-Json) }

[void][IO.Directory]::CreateDirectory($WorkRoot)
$scratchKey = 'Software\amdgpu-wddm-installer-test-components'
$script:ScheduleOldCopies = $false
$script:DryRunMode = $false
$script:NetworkBlocked = $true
# Nothing of this test may reach the machine PATH or the real Start menu.
$script:EnvironmentHive = [Microsoft.Win32.Registry]::CurrentUser
$script:EnvironmentKey = "$scratchKey\Environment"
$script:StartMenuDir = Join-Path $WorkRoot 'startmenu'
$script:StartupDir = Join-Path $WorkRoot 'startup'
Check ($script:EnvironmentHive.Name -like 'HKEY_CURRENT_USER*') "the PATH cases write under HKCU\$($script:EnvironmentKey), never the machine PATH"
$k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("$scratchKey\Environment")
$k.SetValue('Path', 'C:\Windows;C:\Windows\System32', [Microsoft.Win32.RegistryValueKind]::ExpandString)
$k.Close()

# A component registers its own logon entry as a scheduled task named 'BC250 ...'. This test registers no task, and
# the computer it runs on may have one of its own, so the footprint probe looks for a pattern nothing matches.
$script:ComponentTaskPattern = 'BC250 test-components not-registered *'

'the manifest of this package'
$packageManifest = Read-ComponentsManifest $Installer
$fromSource = $false
if ($null -eq $packageManifest -and $Source -and (Test-Path -LiteralPath $Source -PathType Leaf)) {
    # Run against the repository instead of a built package: expand the placeholders as build-release.ps1 does.
    $packageManifest = Get-Content -LiteralPath $Source -Raw | ConvertFrom-Json
    $packageManifest.release = $Release
    if ($packageManifest.PSObject.Properties['comment']) { $packageManifest.PSObject.Properties.Remove('comment') }
    foreach ($c in @($packageManifest.components)) {
        $c.asset.name = ([string]$c.asset.name).Replace('{release}', $Release)
        $c.asset.url_templates = @(@($c.asset.url_templates) | ForEach-Object { ([string]$_).Replace('{release}', $Release) })
    }
    $fromSource = $true
}
Check ($null -ne $packageManifest) "a components manifest was read ($(if ($fromSource) { "from $Source, placeholders expanded as build-release.ps1 does" } else { 'installer\components.json of the package' }))"
if ($null -ne $packageManifest) {
    $p = @(Test-ComponentsManifest $packageManifest)
    foreach ($one in $p) { "  -> $one" }
    Check ($p.Count -eq 0) "it validates ($(@($packageManifest.components).Count) components, $(@($packageManifest.models).Count) models)"
    Check ([string]$packageManifest.release -notmatch '\{') 'the release version is expanded, not a placeholder'
    foreach ($c in @($packageManifest.components)) {
        Check (([string]$c.asset.name) -notmatch '\{') "component $([string]$c.id): the asset name is expanded ($([string]$c.asset.name))"
    }
    foreach ($m in @($packageManifest.models)) {
        Check ((([string]$m.sha256) -match '^[0-9A-Fa-f]{64}$') -and [long]$m.size -gt 0) "model $([string]$m.id): $(Format-ComponentSize ([long]$m.size)), SHA-256 pinned"
        Check ([bool][string]$m.licence_url) "model $([string]$m.id): one licence line ($([string]$m.licence))"
    }
}
if ($Source -and (Test-Path -LiteralPath $Source -PathType Leaf)) {
    $src = Get-Content -LiteralPath $Source -Raw | ConvertFrom-Json
    Check ([string]$src.release -eq '{release}') 'the repository copy keeps {release} for build-release.ps1 to expand'
}

'a manifest the installer must refuse, one message per fault'
$base = $null
if ($null -ne $packageManifest) { $base = Copy-Json $packageManifest }
if ($null -eq $base) { "  (no package manifest: the broken-manifest cases are skipped)" } else {
    $bad = Copy-Json $base; $bad.schema = 'amdgpu-wddm.components/2'
    Check (Matched (Test-ComponentsManifest $bad) 'schema') 'another schema version'
    $bad = Copy-Json $base; $bad.components[0].install_dir = 'tools\elsewhere'
    Check (Matched (Test-ComponentsManifest $bad) 'install_dir') 'an install_dir that is not tools\<id>'
    $bad = Copy-Json $base; $bad.components[0].models = @('no-such-model')
    Check (Matched (Test-ComponentsManifest $bad) 'does not offer') 'a component naming a model that is not offered'
    $bad = Copy-Json $base; $bad.models[0].sha256 = 'PENDING'
    Check (Matched (Test-ComponentsManifest $bad) 'no SHA-256 pin') 'a model without a SHA-256'
    $bad = Copy-Json $base; $bad.components[0].asset.name = 'llama-{release}.zip'
    Check (Matched (Test-ComponentsManifest $bad) 'placeholder') 'an asset name that still holds {release}'
    $bad = Copy-Json $base; $bad.components[0].asset.url_templates = @('http://example.invalid/{name}')
    Check (Matched (Test-ComponentsManifest $bad) 'not https') 'a download address that is not https'
    $bad = Copy-Json $base; $bad.components[0].id = 'Llama'
    Check (Matched (Test-ComponentsManifest $bad) 'component id') 'an id that is not lower case'
    $bad = Copy-Json $base
    $bad.components[0] | Add-Member -NotePropertyName hooks -NotePropertyValue ([pscustomobject]@{ script = '..\outside.ps1'; register = @('register') }) -Force
    Check (Matched (Test-ComponentsManifest $bad) 'relative path inside') 'a hook script outside the component folder'
    $bad = Copy-Json $base
    $bad.components[0] | Add-Member -NotePropertyName hooks -NotePropertyValue ([pscustomobject]@{ script = 'h.ps1'; pull = @('pull') }) -Force
    Check (Matched (Test-ComponentsManifest $bad) '\{tag\}') 'a pull hook without {tag}'
    $bad = Copy-Json $base
    $bad.components[0] | Add-Member -NotePropertyName backend_setting -NotePropertyValue ([pscustomobject]@{ file = 'b.cfg'; key = 'backend'; values = @('a', 'b'); default = 'c' }) -Force
    Check (Matched (Test-ComponentsManifest $bad) 'backend_setting default') 'a backend default that is not one of its values'
    $bad = Copy-Json $base
    $bad.components[0] | Add-Member -NotePropertyName user_models_dir -NotePropertyValue 'C:\store' -Force
    Check (Matched (Test-ComponentsManifest $bad) 'user_models_dir') 'a model store of a component that is not under an environment variable'
    $bad = Copy-Json $base
    $bad.components[0] | Add-Member -NotePropertyName user_models_dir -NotePropertyValue '%LOCALAPPDATA%\..\models' -Force
    Check (Matched (Test-ComponentsManifest $bad) "holds '\.\.'") 'a model store of a component that climbs out of the profile'
}

'a component without a pin is refused, and nothing is downloaded'
$one = [pscustomobject]@{
    schema = $script:ComponentsSchema; release = '0.0.0.0-test.1'; models_dir = (Join-Path $WorkRoot 'models')
    components = @([pscustomobject]@{ id = 'demo'; title = 'Demo extra'; question = 'Install the demo extra?'; summary = 'a demo'
            version = '1'; default = $true; install_dir = 'tools\demo'; licence = 'MIT'; licence_url = 'https://example.com/LICENSE'
            asset = [pscustomobject]@{ name = 'demo.zip'; size = 0; sha256 = 'PENDING'; url_templates = @('https://example.com/{release}/{name}') }
            zip_root = 'demo'; entry = 'demo.exe'; path_entry = $true; models = @('tiny') })
    models = @([pscustomobject]@{ id = 'tiny'; title = 'Tiny model'; file = 'tiny.gguf'; size = 11; licence = 'MIT'
            url = 'https://example.com/tiny.gguf'
            licence_url = 'https://example.com/model'; ollama_tag = 'tiny:1'; needs_12gb = $false; note = ''
            sha256 = 'AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDEEEEEEEEFFFFFFFF00000000111111112' })
}
# The fixture model's SHA-256 must be a real one: it is written below from the file's own bytes.
$modelBytes = [Text.Encoding]::ASCII.GetBytes('tiny model!')
$modelSrcDir = Join-Path $WorkRoot 'offline'
[void][IO.Directory]::CreateDirectory($modelSrcDir)
[IO.File]::WriteAllBytes((Join-Path $modelSrcDir 'tiny.gguf'), $modelBytes)
$one.models[0].sha256 = (Get-Sha256 (Join-Path $modelSrcDir 'tiny.gguf'))
$one.models[0].size = $modelBytes.Length
Check (@(Test-ComponentsManifest $one).Count -eq 0) 'the fixture manifest validates'
Check ((Get-ComponentPinProblem $one.components[0]) -match 'no SHA-256 in this package') 'a PENDING asset SHA-256 is a missing pin'
$staging = Join-Path $WorkRoot 'staging'
$msg = $null
try { [void](Get-ComponentAssetStaged -Manifest $one -Component $one.components[0] -Staging $staging) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'no SHA-256 in this package') 'staging a component without a pin is refused'
Check (-not (Test-Path -LiteralPath (Join-Path $staging 'demo.zip'))) 'nothing was staged for it'
$one.components[0].asset.sha256 = ('0' * 64)
Check ((Get-ComponentPinProblem $one.components[0]) -match 'no size') 'a pin without a size is a missing pin'

'the switch combinations'
$r = Resolve-ComponentRequest -Manifest $one -Requested @() -NoComponents $true -CanAsk $true -DefaultsWithoutAsking $true
Check ($r.mode -eq 'none' -and @($r.ids).Count -eq 0) '-NoComponents: none, and no question'
$r = Resolve-ComponentRequest -Manifest $one -Requested @('demo') -NoComponents $false -CanAsk $true -DefaultsWithoutAsking $false
Check ($r.mode -eq 'list' -and (@($r.ids) -join ',') -eq 'demo') '-Components demo: exactly that one, no question'
$r = Resolve-ComponentRequest -Manifest $one -Requested @('demo,nope; demo') -NoComponents $false -CanAsk $true -DefaultsWithoutAsking $false
Check ((@($r.ids) -join ',') -eq 'demo' -and (@($r.unknown) -join ',') -eq 'nope') 'a list with a comma, a space, a repeat and an unknown id'
$r = Resolve-ComponentRequest -Manifest $one -Requested @() -NoComponents $false -CanAsk $true -DefaultsWithoutAsking $false
Check ($r.mode -eq 'ask') 'no switch and a console: the question is asked'
$r = Resolve-ComponentRequest -Manifest $one -Requested @() -NoComponents $false -CanAsk $false -DefaultsWithoutAsking $true
Check ($r.mode -eq 'default' -and (@($r.ids) -join ',') -eq 'demo') 'unattended without a switch: the components whose default is yes'
$r = Resolve-ComponentRequest -Manifest $one -Requested @() -NoComponents $false -CanAsk $false -DefaultsWithoutAsking $false
Check ($r.mode -eq 'none' -and @($r.ids).Count -eq 0) 'the setup window without a switch: nothing is consented, nothing is installed'
$r = Resolve-ModelRequest -Manifest $one -Requested @() -NoModels $false -CanAsk $false -ComponentIds @('demo')
Check ($r.mode -eq 'none' -and @($r.ids).Count -eq 0) 'unattended: no model is chosen by itself'
$r = Resolve-ModelRequest -Manifest $one -Requested @() -NoModels $false -CanAsk $true -ComponentIds @('demo')
Check ($r.mode -eq 'ask') 'with a console the model question is asked'
$r = Resolve-ModelRequest -Manifest $one -Requested @('tiny') -NoModels $true -CanAsk $true -ComponentIds @('demo')
Check ($r.mode -eq 'none' -and @($r.ids).Count -eq 0) '-NoModels wins over -Models'
$r = Resolve-ModelRequest -Manifest $one -Requested @('tiny', 'other') -NoModels $false -CanAsk $false -ComponentIds @('demo')
Check ((@($r.ids) -join ',') -eq 'tiny' -and (@($r.unknown) -join ',') -eq 'other') '-Models with an unknown id'
$r = Resolve-ModelRequest -Manifest $one -Requested @('tiny') -NoModels $false -CanAsk $false -ComponentIds @()
Check ((@($r.ids).Count -eq 0) -and ((@($r.unusable) -join ',') -eq 'tiny')) 'no component chosen: no model is offered, and a -Models that was given is reported'
$r = Resolve-ModelRequest -Manifest $one -Requested @() -NoModels $false -CanAsk $true -ComponentIds @()
Check ($r.mode -eq 'none') 'and the model question is not asked when no extra can use an answer'
$two = Copy-Json $one
$two.components[0].models = @()
$r = Resolve-ModelRequest -Manifest $two -Requested @('tiny') -NoModels $false -CanAsk $false -ComponentIds @('demo')
Check ((@($r.unusable) -join ',') -eq 'tiny') 'a model that the chosen component cannot use is reported, not downloaded'
Check ((Get-ComponentModelRoute $one.components[0]) -eq 'gguf') 'a component without a pull hook reads GGUF files'

'what one model costs on the disk, with the extras that were chosen'
$ggufOnly = Copy-Json $one.components[0]
$pullOnly = Copy-Json $one.components[0]
$pullOnly | Add-Member -NotePropertyName hooks -NotePropertyValue ([pscustomobject]@{ script = 'h.ps1'; pull = @('pull', '{tag}') }) -Force
Check ($null -eq (Get-ModelCostText $one.models[0] @($ggufOnly) @())) 'one route: the size in the list is the whole cost, and no second line is printed'
$cost = Get-ModelCostText $one.models[0] @($ggufOnly) @($pullOnly)
"  -> $cost"
Check (($cost -match 'again') -and ($cost -match 'in all')) 'both routes: the question says the model is stored twice and gives the sum'

'the offline folder'
$zipDir = Join-Path $WorkRoot 'zipsrc\demo'
[void][IO.Directory]::CreateDirectory($zipDir)
[IO.File]::WriteAllText((Join-Path $zipDir 'demo.exe'), 'not really an exe')
[IO.File]::WriteAllText((Join-Path $zipDir 'README.md'), 'how to start the demo')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Join-Path $modelSrcDir 'demo.zip'
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
[IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $WorkRoot 'zipsrc'), $zipPath)
$one.components[0].asset.sha256 = (Get-Sha256 $zipPath)
$one.components[0].asset.size = (Get-Item -LiteralPath $zipPath).Length
Check ($null -eq (Get-ComponentPinProblem $one.components[0])) 'the fixture component now has a pin'
Check (@(Test-ComponentSetDir $one @('demo') $modelSrcDir).Count -eq 0) 'the offline folder holds the pinned zip'
Check (@(Test-ModelSetDir $one @('tiny') $modelSrcDir).Count -eq 0) 'the offline folder holds the pinned model'
$emptyDir = Join-Path $WorkRoot 'offline-empty'
[void][IO.Directory]::CreateDirectory($emptyDir)
Check (Matched (Test-ComponentSetDir $one @('demo') $emptyDir) 'missing demo.zip') 'an offline folder without the zip says what is missing'
$changedDir = Join-Path $WorkRoot 'offline-changed'
[void][IO.Directory]::CreateDirectory($changedDir)
Copy-Item -LiteralPath $zipPath -Destination $changedDir
[IO.File]::AppendAllText((Join-Path $changedDir 'demo.zip'), 'x')
Check (Matched (Test-ComponentSetDir $one @('demo') $changedDir) 'changed demo.zip') 'a changed zip in an offline folder is reported'
$msg = $null
try { [void](Get-ComponentAssetStaged -Manifest $one -Component $one.components[0] -Staging $staging -FromDir $changedDir) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'this release pins') 'staging a changed zip from a folder is refused'
Check (-not (Test-Path -LiteralPath (Join-Path $staging 'demo.zip'))) 'the refused zip is not left in staging'
$msg = $null
try { [void](Get-ComponentAssetStaged -Manifest $one -Component $one.components[0] -Staging $staging) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'network unavailable') 'with no network the download says so and stages nothing'
$staged = Get-ComponentAssetStaged -Manifest $one -Component $one.components[0] -Staging $staging -FromDir $modelSrcDir
Check ((Get-Sha256 $staged) -eq ([string]$one.components[0].asset.sha256).ToUpperInvariant()) 'the good zip stages from the offline folder with the pinned SHA256'

'a folder prepared by prepare-offline.cmd: components\ and models\ side by side'
# The layout prepare-offline.ps1 really writes, which is not the flat folder of the cases above: the component zips
# in components\ and the model files in models\. An install from that folder must find both.
$setRoot = Join-Path $WorkRoot 'prepared'
$setComponents = Get-SetComponentDir $setRoot
$setModels = Get-SetModelDir $setRoot
[void][IO.Directory]::CreateDirectory($setComponents)
[void][IO.Directory]::CreateDirectory($setModels)
Copy-Item -LiteralPath $zipPath -Destination $setComponents
Copy-Item -LiteralPath (Join-Path $modelSrcDir 'tiny.gguf') -Destination $setModels
Check ((Get-PackageComponentDir $setRoot) -eq $setComponents) 'the install finds the component zips in components\'
Check ((Get-PackageModelDir $setRoot) -eq $setModels) 'and the model files in models\'
Check ((Get-ComponentSourceDir $setRoot) -eq $setComponents) 'the zips of the prepared root are in its components\ subfolder, which -ComponentDir <root> resolves to'
Check ((Get-ComponentSourceDir $modelSrcDir) -eq $modelSrcDir) 'a folder that holds the zips themselves stays itself'
Check ((Get-ModelSourceDir $setRoot) -eq $setModels) 'the models folder of the prepared root is its models\ subfolder'
Check ((Get-ModelSourceDir $setComponents) -eq $setModels) 'and of its components\ subfolder, which is what install.ps1 resolves, the same one'
Check ((Get-ModelSourceDir $modelSrcDir) -eq $modelSrcDir) 'a folder put together by hand, with the zips and the models together, stays itself'
Check (@(Test-ComponentSetDir $one @('demo') $setComponents).Count -eq 0) 'the prepared folder holds the pinned zip'
Check (@(Test-ModelSetDir $one @('tiny') (Get-ModelSourceDir $setComponents)).Count -eq 0) 'the prepared folder holds the pinned model where the install looks for it'
Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
$setStaged = Get-ComponentAssetStaged -Manifest $one -Component $one.components[0] -Staging $staging -FromDir (Get-PackageComponentDir $setRoot)
Check ((Get-Sha256 $setStaged) -eq ([string]$one.components[0].asset.sha256).ToUpperInvariant()) 'the zip stages out of the prepared folder'
$setModelsOut = Join-Path $WorkRoot 'models-from-prepared'
Check ((Get-ModelFile -Model $one.models[0] -ModelsDir $setModelsOut -FromDir (Get-ModelSourceDir $setComponents)) -eq 'copied') 'the model is taken from the prepared folder, not downloaded'
Remove-Item -LiteralPath $setModelsOut, $setRoot -Recurse -Force -ErrorAction SilentlyContinue

'a file that is not in the folder is a miss, not a fault: it is downloaded instead'
# The installer tells the user that what is missing from the folder is downloaded. With no network that download
# fails, and the message says so instead of naming the folder: the folder route was left, as it must be.
Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
$msg = $null
try { [void](Get-ComponentAssetStaged -Manifest $one -Component $one.components[0] -Staging $staging -FromDir $emptyDir) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'network unavailable') 'a zip missing from the folder reaches the download route'
Check (-not (Test-Path -LiteralPath (Join-Path $staging 'demo.zip'))) 'and with no network nothing is staged for it'
$msg = $null
try { [void](Get-ModelFile -Model $one.models[0] -ModelsDir (Join-Path $WorkRoot 'models-miss') -FromDir $emptyDir) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'network unavailable') 'a model missing from the folder reaches the download route as well'
Check (-not (Test-Path -LiteralPath (Join-Path $WorkRoot 'models-miss\tiny.gguf'))) 'and nothing is kept for it'
# The other half: with the download route working, the missing file is fetched. The large-file route is replaced for
# this one case, because this test uses no network at all.
$realLargeFile = ${function:Get-LargeFile}
function Get-LargeFile { param([string]$Url, [string]$Destination, [int]$TimeoutSec = 0)
    [IO.File]::WriteAllBytes($Destination, [Text.Encoding]::ASCII.GetBytes('tiny model!'))
    return (Get-Item -LiteralPath $Destination).Length }
$state3 = Get-ModelFile -Model $one.models[0] -ModelsDir (Join-Path $WorkRoot 'models-miss') -FromDir $emptyDir
${function:Get-LargeFile} = $realLargeFile
Check ($state3 -eq 'downloaded') 'with the download working, a model missing from the folder is downloaded and checked'
Check ((Get-Sha256 (Join-Path $WorkRoot 'models-miss\tiny.gguf')) -eq ([string]$one.models[0].sha256).ToUpperInvariant()) 'and its SHA256 is checked like every other download'
Remove-Item -LiteralPath (Join-Path $WorkRoot 'models-miss') -Recurse -Force -ErrorAction SilentlyContinue

'a zip that would write outside the component folder is refused'
Check ((@(Get-ZipUnsafeEntries @('demo/demo.exe', 'ok.txt')).Count -eq 0)) 'ordinary entries are fine'
Check ((@(Get-ZipUnsafeEntries @('..\..\windows\system32\evil.dll')).Count -eq 1)) 'an entry that climbs out is unsafe'
Check ((@(Get-ZipUnsafeEntries @('C:\windows\evil.dll', '/etc/passwd')).Count -eq 2)) 'an absolute entry is unsafe'

'a dry run changes nothing'
$installRoot = Join-Path $WorkRoot 'root'
$script:DryRunMode = $true
[void](Install-ComponentFiles -Manifest $one -Component $one.components[0] -Zip $staged -InstallRoot $installRoot -Staging $staging -ModelsDir (Join-Path $WorkRoot 'models'))
$script:DryRunMode = $false
Check (-not (Test-Path -LiteralPath (Join-Path $installRoot 'tools\demo'))) 'the dry run wrote no component folder'
Check (-not (Test-PathEntry (Get-MachinePath).value (Join-Path $installRoot 'tools\demo'))) 'the dry run added no PATH entry'
Check (-not (Test-Path -LiteralPath $script:StartMenuDir)) 'the dry run wrote no shortcut'

'the install'
# A hook script of the fixture component: it writes a marker and prints a line in another language, so the test can
# show that the installer acts on the exit code and never on the text (issue #35).
$one.components[0] | Add-Member -NotePropertyName hooks -NotePropertyValue ([pscustomobject]@{
        script = 'demo-hook.ps1'; register = @('register'); unregister = @('unregister'); pull = @('pull', '-Model', '{tag}') }) -Force
$one.components[0] | Add-Member -NotePropertyName backend_setting -NotePropertyValue ([pscustomobject]@{
        file = 'demo-backend.cfg'; key = 'backend'; values = @('vulkan', 'hip'); default = 'vulkan' }) -Force
# A component that keeps the models in its own format, in the user profile. The fixture points the store at the work
# folder through an environment variable of this test, so nothing of the account's own data is touched.
$env:BC250_TEST_STORE = $WorkRoot
$one.components[0] | Add-Member -NotePropertyName user_models_dir -NotePropertyValue '%BC250_TEST_STORE%\own-store' -Force
$ownStore = Join-Path $WorkRoot 'own-store'
# A line of prose in another language, in ASCII so this file stays ASCII like the installer's own: the installer must
# act on the exit code and never on this text.
$foreign = 'La operacion no se pudo completar'
$hookBody = @(
    'param([Parameter(Position=0)][string]$Verb, [string]$Model)',
    "Write-Output '$foreign'",
    '$marker = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "hook-$Verb.txt"',
    '[IO.File]::WriteAllText($marker, [string]$Model)',
    'if ($Verb -eq "fail") { exit 3 }',
    'exit 0')
# The hook ships inside the zip, so it is in the component folder after the install.
[IO.File]::WriteAllLines((Join-Path $zipDir 'demo-hook.ps1'), [string[]]$hookBody)
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
[IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $WorkRoot 'zipsrc'), $zipPath)
$one.components[0].asset.sha256 = (Get-Sha256 $zipPath)
$one.components[0].asset.size = (Get-Item -LiteralPath $zipPath).Length
foreach ($one2 in @(Test-ComponentsManifest $one)) { "  -> $one2" }
Check (@(Test-ComponentsManifest $one).Count -eq 0) 'the fixture manifest with a hook and a backend setting validates'
Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
$staged = Get-ComponentAssetStaged -Manifest $one -Component $one.components[0] -Staging $staging -FromDir $modelSrcDir
$modelsDir = Join-Path $WorkRoot 'models'
# The shortcut an older release wrote under the old name ("amdgpu-wddm <title>"), pointing at the component's own
# program: the install over it writes the new name and removes the old one.
$dir = Join-Path $installRoot 'tools\demo'
$oldMenu = Join-Path $script:StartMenuDir 'amdgpu-wddm Demo extra.lnk'
[void](New-ComponentShortcut -Link $oldMenu -Target (Join-Path $dir 'demo.exe') -WorkDir $dir -Hidden $false)
Check (Test-Path -LiteralPath $oldMenu) 'the old-named shortcut of an older release is there before the install'
$record = Install-ComponentFiles -Manifest $one -Component $one.components[0] -Zip $staged -InstallRoot $installRoot -Staging $staging -ModelsDir $modelsDir
Check ((Split-Path $record.start_menu -Leaf) -eq 'Nagi Demo extra.lnk') "the Start menu shortcut carries the name Nagi ($(Split-Path $record.start_menu -Leaf))"
Check (-not (Test-Path -LiteralPath $oldMenu)) 'the install removed the old name, which pointed into the component folder'
$names = Get-ComponentShortcutPaths $one.components[0]
Check ($null -eq $names.startup -and @($names.legacy).Count -eq 1) 'a component without a server launcher has one new name and one old name'
$allNames = Get-ComponentShortcutPaths ([pscustomobject]@{ title = 'Demo extra' }) -All
Check (((@($allNames.start_menu, $allNames.startup) + @($allNames.legacy) | ForEach-Object { Split-Path $_ -Leaf }) -join '|') -eq 'Nagi Demo extra.lnk|Nagi Demo extra server.lnk|amdgpu-wddm Demo extra.lnk|amdgpu-wddm Demo extra server.lnk') 'a record of the state knows the four names its title can carry'
Check (Test-Path -LiteralPath (Join-Path $dir 'demo.exe')) 'the zip_root contents are in <install root>\tools\demo'
Check (-not (Test-Path -LiteralPath (Join-Path $dir 'demo'))) 'the zip_root folder itself is not repeated inside it'
Check ((Get-Content -LiteralPath (Join-Path $dir 'demo-backend.cfg') -Raw).Trim() -eq 'backend=vulkan') 'the backend setting is one line in the component folder'
Check (Test-Path -LiteralPath (Join-Path $dir 'hook-register.txt')) 'the register hook ran'
Check (Test-PathEntry (Get-MachinePath).value $dir) 'the component folder is on the PATH of the scratch hive'
Check (Test-Path -LiteralPath $record.start_menu) "a Start menu shortcut was written ($(Split-Path $record.start_menu -Leaf))"
Check ($record.path_entry -eq $dir -and $record.backend -eq 'vulkan' -and $record.hooks -eq 'demo-hook.ps1') 'the state record names the folder, the backend and the hook'
Check ($record.user_models_dir -eq $ownStore) 'the state record holds the component''s own model store, with the variable expanded'
Check ((Get-MachinePath).value -like 'C:\Windows;C:\Windows\System32;*') 'the entries that were there before are untouched'

'zip_root "*": the one folder the zip holds, whatever its name'
$starSrc = Join-Path $WorkRoot 'starsrc'
$starRoot = Join-Path $starSrc 'demo-b86d2f07-tester.27'
[void][IO.Directory]::CreateDirectory($starRoot)
[IO.File]::WriteAllText((Join-Path $starRoot 'demo.exe'), 'fixture')
$starZip = Join-Path $modelSrcDir 'demo-star.zip'
if (Test-Path -LiteralPath $starZip) { Remove-Item -LiteralPath $starZip -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($starSrc, $starZip)
$starDst = Join-Path $WorkRoot 'star-out'
[void](Expand-ComponentZip -Zip $starZip -Destination $starDst -ZipRoot '*' -Staging $staging)
Check (Test-Path -LiteralPath (Join-Path $starDst 'demo.exe')) 'a folder whose name carries a commit needs no name in components.json'
Check (-not (Test-Path -LiteralPath (Join-Path $starDst 'demo-b86d2f07-tester.27'))) 'that folder itself is not repeated inside the component folder'
[IO.File]::WriteAllText((Join-Path $starSrc 'loose.txt'), 'a second entry at the top level')
if (Test-Path -LiteralPath $starZip) { Remove-Item -LiteralPath $starZip -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($starSrc, $starZip)
$msg = $null
try { [void](Expand-ComponentZip -Zip $starZip -Destination (Join-Path $WorkRoot 'star-out2') -ZipRoot '*' -Staging $staging) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'does not hold exactly one folder') 'a zip with more than one entry at its top level is refused'
Remove-Item -LiteralPath $starSrc, $starDst -Recurse -Force -ErrorAction SilentlyContinue

'a hook is judged by its exit code, not by its output (issue #35)'
$failing = Copy-Json $one.components[0]
$failing.hooks.register = @('fail')
$out = Invoke-ComponentHook -Component $failing -Dir $dir -Verb 'register' -TimeoutSeconds 60
Check ($out.ran -and $out.code -eq 3) "the exit code comes back unchanged (3), whatever the script printed"
Check ($out.text -match [regex]::Escape($foreign)) 'the output is kept for the log and never parsed'
$out = Invoke-ComponentHook -Component $one.components[0] -Dir (Join-Path $WorkRoot 'nowhere') -Verb 'register' -TimeoutSeconds 60
Check (-not $out.ran -and $out.text -match 'is not there') 'a hook script that is gone is reported, not run'
$out = Invoke-ComponentHook -Component $one.components[0] -Dir $dir -Verb 'status' -TimeoutSeconds 60
Check (-not $out.ran -and $out.text -match "no 'status' hook") 'a verb the component does not have is not invented'

'the models: a component with a pull hook pulls the tag, one without it gets the GGUF file'
Check ((Get-ComponentModelRoute $one.components[0]) -eq 'pull') 'the fixture component with a pull hook takes the pull route'
$pullRecords = @(Install-ComponentModels -Manifest $one -Components @($one.components[0]) -ModelIds @('tiny') -InstallRoot $installRoot -ModelsDir $modelsDir -FromDir $modelSrcDir)
Check (Test-Path -LiteralPath (Join-Path $dir 'hook-pull.txt')) 'the pull hook ran for the component that pulls by tag'
Check ((Get-Content -LiteralPath (Join-Path $dir 'hook-pull.txt') -Raw) -eq 'tiny:1') 'the pull hook got the registry tag of the model'
Check (-not (Test-Path -LiteralPath (Join-Path $modelsDir 'tiny.gguf'))) 'no GGUF file was downloaded for a component that pulls by tag'
Check (@($pullRecords | Where-Object { ([string]$_.state) -like 'pulled by *' }).Count -eq 1) 'the record says the tag was pulled, and by which extra'
# The same models, for a component that reads GGUF files (our llama.cpp build).
$ggufComponent = Copy-Json $one.components[0]
$ggufComponent.PSObject.Properties.Remove('hooks')
Check ((Get-ComponentModelRoute $ggufComponent) -eq 'gguf') 'a component without a pull hook reads GGUF files from the models folder'
$modelRecords = @(Install-ComponentModels -Manifest $one -Components @($ggufComponent) -ModelIds @('tiny') -InstallRoot $installRoot -ModelsDir $modelsDir -FromDir $modelSrcDir)
Check (Test-Path -LiteralPath (Join-Path $modelsDir 'tiny.gguf')) "the model file is in $modelsDir"
Check (@($modelRecords | Where-Object { $_.state -eq 'copied' }).Count -eq 1) 'the model record says it came from the offline folder'
$again = @(Install-ComponentModels -Manifest $one -Components @($ggufComponent) -ModelIds @('tiny') -InstallRoot $installRoot -ModelsDir $modelsDir -FromDir $modelSrcDir)
Check (@($again | Where-Object { $_.state -eq 'present' }).Count -eq 1) 'a model that is already there with the pinned SHA256 is kept, not downloaded again'
[IO.File]::AppendAllText((Join-Path $modelsDir 'tiny.gguf'), 'x')
$msg = $null
try { [void](Get-ModelFile -Model $one.models[0] -ModelsDir $modelsDir -FromDir $emptyDir) } catch { $msg = $_.Exception.Message }
"  -> $msg"
Check ($msg -match 'network unavailable') 'a model that is neither there nor in the folder is downloaded, and with no network that is what the message says'
Check (-not (Test-Path -LiteralPath (Join-Path $modelsDir 'tiny.gguf'))) 'the file that did not match the pin was not kept'
[void](Get-ModelFile -Model $one.models[0] -ModelsDir $modelsDir -FromDir $modelSrcDir)

'the uninstall'
$state = [pscustomobject]@{ install_root = $installRoot; components_installed = @($record); models_installed = @($modelRecords); models_dir = $modelsDir }
# Two old-named shortcuts left by an older release: one of ours (into the component folder), one with a foreign target.
$oldServer = Join-Path $script:StartupDir 'amdgpu-wddm Demo extra server.lnk'
$foreignExe = Join-Path $WorkRoot 'foreign\demo.exe'
[void](New-ComponentShortcut -Link $oldMenu -Target (Join-Path $dir 'demo.exe') -WorkDir $dir -Hidden $false)
[void](New-ComponentShortcut -Link $oldServer -Target $foreignExe -WorkDir (Split-Path $foreignExe) -Hidden $true)
$footBefore = @(Get-ComponentFootprint $state $false | Where-Object { $_.item -eq 'component shortcuts' })[0]
Check ($footBefore.present -and $footBefore.detail -like '*Nagi Demo extra.lnk*' -and $footBefore.detail -like '*amdgpu-wddm Demo extra.lnk*') "the footprint lists the new and the old name of ours ($($footBefore.detail))"
Check ($footBefore.detail -notlike '*server.lnk*') 'the footprint does not count the old name with a foreign target'
Check ((Get-StateModelsDir $state) -eq $modelsDir) 'the state says which models folder this installation used'
Invoke-ComponentUnregister $state $one
Check (Test-Path -LiteralPath (Join-Path $dir 'hook-unregister.txt')) 'each extra undid its own registration first, with its own script'
Remove-ComponentRegistrations $state
Check (-not (Test-PathEntry (Get-MachinePath).value $dir)) 'the PATH entry is gone'
Check ((Get-MachinePath).value -eq 'C:\Windows;C:\Windows\System32') 'and nothing else of the PATH changed'
Check (-not (Test-Path -LiteralPath $record.start_menu)) 'the Start menu shortcut is gone'
Check (-not (Test-Path -LiteralPath $oldMenu)) 'the uninstall removed the old name of ours too'
Check (Test-Path -LiteralPath $oldServer) 'the uninstall kept the old name that points at a foreign program'
Check (@(Get-ComponentFootprint $state $false | Where-Object { $_.item -eq 'component shortcuts' })[0].present -eq $false) 'the footprint sees no shortcut of ours after the uninstall'
Remove-Item -LiteralPath $oldServer -Force
# The component's own store, as its server would have written it.
[void][IO.Directory]::CreateDirectory($ownStore)
[IO.File]::WriteAllText((Join-Path $ownStore 'blobs.bin'), 'a model in the component''s own format')
Remove-ComponentModels $state $false
Check (Test-Path -LiteralPath (Join-Path $modelsDir 'tiny.gguf')) 'without -RemoveModels the downloaded model stays: it is the user''s data'
Check (Test-Path -LiteralPath $ownStore) 'without -RemoveModels the component''s own model store stays as well'
$foot = @(Get-ComponentFootprint $state $false)
$modelRow = @($foot | Where-Object { $_.item -eq 'downloaded models' })[0]
Check ($modelRow.present -and $modelRow.kept) 'the footprint lists the models as kept, not as left behind'
$storeRow = @($foot | Where-Object { $_.item -eq 'models of a component in its own format' })[0]
Check ($null -ne $storeRow -and $storeRow.present -and $storeRow.kept) 'the footprint lists the component''s own model store as kept'
Check (@($foot | Where-Object { $_.item -eq 'component PATH entries' })[0].present -eq $false) 'the footprint sees no PATH entry of ours'
Check (@($foot | Where-Object { -not $_.known }).Count -eq 0) 'every footprint row could be read'
Remove-ComponentModels $state $true
Check (-not (Test-Path -LiteralPath (Join-Path $modelsDir 'tiny.gguf'))) 'with -RemoveModels the models are removed'
Check (-not (Test-Path -LiteralPath $ownStore)) 'with -RemoveModels the component''s own model store is removed as well'
# The component folder itself goes with the install root, as uninstall.ps1 removes it.
Remove-PathOrSchedule $installRoot
$foot = @(Get-ComponentFootprint $state $true)
Check (@($foot | Where-Object { $_.present -and -not $_.kept }).Count -eq 0) 'nothing of the extras is left after the install root is removed'

Remove-Item Env:\BC250_TEST_STORE -ErrorAction SilentlyContinue
[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($scratchKey, $false)
Check (-not [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($scratchKey)) 'the scratch registry key is removed'
Remove-Item -LiteralPath $staging, $modelSrcDir, $changedDir, $emptyDir, (Join-Path $WorkRoot 'zipsrc'), $script:StartMenuDir, $script:StartupDir -Recurse -Force -ErrorAction SilentlyContinue

if ($fail) { "$fail component check(s) failed"; exit 1 }
'component checks passed'
exit 0
