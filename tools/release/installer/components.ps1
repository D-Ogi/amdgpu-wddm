# The optional components of the tester release: our llama.cpp build and our Ollama build, and the models the user
# may want with them. Windows PowerShell 5.1 syntax only, like the rest of the installer.
#
# The package carries no component binary. installer\components.json pins every component's release asset and every
# offered model file with its SHA-256, and this module downloads them (or takes them from a folder), checks each
# SHA-256 and only then unpacks or keeps the file. A component without a usable pin is refused and nothing is
# downloaded for it: nothing is ever installed unchecked.
#
# A component is optional in every sense. The install asks one plain question per component, a skipped, refused or
# failed component never fails the driver installation, and the uninstaller removes a component's files and every
# registration it made. The downloaded models are the user's data: they stay unless uninstall runs -RemoveModels.
#
# One block here has no component behind it yet: 'backend_setting' (the validation below and the write in
# Install-ComponentFiles) writes one line into a component's own folder for a component that reads its backend from
# a file. Neither component of this release declares it - our Ollama build keeps that choice in its own per-user
# setting, which its 'register' hook writes - so the block is for the next component that needs it, not dead weight
# somebody should assume the Ollama backend goes through.

$script:ComponentsSchema    = 'amdgpu-wddm.components/1'
$script:ComponentsFileName  = 'components.json'
# The models go outside the install root: they are the user's data, they are large, and they survive an uninstall.
# Public Documents is writable by every user of the computer and easy to find in File Explorer.
$script:ModelsDirDefault    = 'C:\Users\Public\Documents\amdgpu-wddm\models'
$script:ComponentSetDir     = 'components'          # in a prepared offline folder: the component assets
$script:ModelSetDir         = 'models'              # in a prepared offline folder: the model files
$script:ComponentIdPattern  = '^[a-z][a-z0-9.-]{1,23}$'
# A component's own logon entry is a scheduled task whose name starts like this, so the uninstaller's footprint list
# can see one that a failed unregister left behind (component contract 3 and 7). The pattern is read only: it is
# reported, never unregistered here, so another task of ours that it also matches is only named in the report.
$script:ComponentTaskPattern = 'BC250 *'
$script:ComponentTries      = 3
$script:ComponentTimeoutSec = 900                   # one asset; the models get their own, longer, route (curl.exe)
$script:StartupDir          = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\StartUp'
$script:StartMenuDir        = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'

# ---------------------------------------------------------------------------------------------------------------
# The manifest. Pure reading and pure checks: every problem is a line of text, nothing throws on bad input.

function Get-ComponentsManifestPath([string]$InstallerDir) { return (Join-Path $InstallerDir $script:ComponentsFileName) }

# $null when this release has no components at all (no components.json). Otherwise the parsed manifest.
function Read-ComponentsManifest([string]$InstallerDir) {
    $p = Get-ComponentsManifestPath $InstallerDir
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $null }
    return (Get-Content -LiteralPath $p -Raw | ConvertFrom-Json)
}

# Pure: everything wrong with a components manifest, one line each. An empty array means it can be used.
function Test-ComponentsManifest($Manifest) {
    $bad = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Manifest) { $bad.Add('no manifest'); return $bad.ToArray() }
    if ([string]$Manifest.schema -ne $script:ComponentsSchema) { $bad.Add("schema '$([string]$Manifest.schema)', expected '$($script:ComponentsSchema)'") }
    if (-not [string]$Manifest.release) { $bad.Add('no release version') }
    $modelIds = @(@($Manifest.models) | ForEach-Object { [string]$_.id })
    $seen = New-Object System.Collections.Generic.List[string]
    foreach ($c in @($Manifest.components)) {
        $id = [string]$c.id
        if ($id -cnotmatch $script:ComponentIdPattern) { $bad.Add("component id '$id' is not <lower-case letters, digits, dot, hyphen>"); continue }
        if ($seen.Contains($id)) { $bad.Add("component id '$id' twice"); continue }
        $seen.Add($id)
        if (-not [string]$c.title) { $bad.Add("component $id has no title") }
        if (-not [string]$c.question) { $bad.Add("component $id has no question") }
        if ([string]$c.install_dir -ne "tools\$id") { $bad.Add("component $id install_dir '$([string]$c.install_dir)', expected 'tools\$id'") }
        if (-not [string]$c.licence -or -not [string]$c.licence_url) { $bad.Add("component $id has no licence line") }
        $a = $c.asset
        if ($null -eq $a) { $bad.Add("component $id has no asset"); continue }
        if (-not [string]$a.name -or ([string]$a.name) -match '[\\/]') { $bad.Add("component $id asset name '$([string]$a.name)' is empty or a path") }
        if (([string]$a.name) -match '[{}]') { $bad.Add("component $id asset name '$([string]$a.name)' still holds a placeholder: build-release.ps1 expands {release}") }
        if (-not @($a.url_templates).Count) { $bad.Add("component $id has no asset url_templates") }
        foreach ($t in @($a.url_templates)) {
            if (([string]$t) -notmatch '\{name\}') { $bad.Add("component $id url template '$t' has no {name}") }
            if (([string]$t) -notmatch '^https://') { $bad.Add("component $id url template '$t' is not https") }
        }
        foreach ($m in @($c.models)) { if ($modelIds -notcontains [string]$m) { $bad.Add("component $id names model '$m', which the manifest does not offer") } }
        $hk = $c.hooks
        if ($null -ne $hk) {
            if (-not [string]$hk.script -or ([string]$hk.script) -match '^[\\/]|\.\.|:') { $bad.Add("component $id hooks script '$([string]$hk.script)' must be a relative path inside the component folder") }
            elseif (([string]$hk.script) -notmatch '\.ps1$') { $bad.Add("component $id hooks script '$([string]$hk.script)' is not a .ps1") }
            foreach ($v in @('register', 'unregister', 'status', 'pull')) {
                $a2 = $hk.$v
                if ($null -eq $a2) { continue }
                if (-not @($a2).Count) { $bad.Add("component $id hook '$v' has no arguments") }
            }
            if ($null -ne $hk.pull -and (@($hk.pull) -join ' ') -notmatch '\{tag\}') { $bad.Add("component $id hook 'pull' has no {tag} argument") }
        }
        # A component that keeps the models in its own format has its own store in the user profile, next to the
        # GGUF folder that the manifest names for every component. uninstall.cmd -RemoveModels clears both.
        $um = [string]$c.user_models_dir
        if ($um) {
            if ($um -notmatch '^%[A-Za-z_][A-Za-z0-9_]*%\\') { $bad.Add("component $id user_models_dir '$um' must start with an environment variable, as in '%LOCALAPPDATA%\<folder>'") }
            elseif ($um -match '\.\.') { $bad.Add("component $id user_models_dir '$um' holds '..'") }
        }
        $bs = $c.backend_setting
        if ($null -ne $bs) {
            if (-not [string]$bs.file -or ([string]$bs.file) -match '^[\\/]|\.\.|:') { $bad.Add("component $id backend_setting file '$([string]$bs.file)' must be a relative path inside the component folder") }
            if (-not [string]$bs.key) { $bad.Add("component $id backend_setting has no key") }
            if (-not @($bs.values).Count) { $bad.Add("component $id backend_setting has no values") }
            if (@($bs.values) -notcontains [string]$bs.default) { $bad.Add("component $id backend_setting default '$([string]$bs.default)' is not one of its values") }
        }
    }
    $seenM = New-Object System.Collections.Generic.List[string]
    foreach ($m in @($Manifest.models)) {
        $id = [string]$m.id
        if ($id -cnotmatch $script:ComponentIdPattern) { $bad.Add("model id '$id' is not <lower-case letters, digits, dot, hyphen>"); continue }
        if ($seenM.Contains($id)) { $bad.Add("model id '$id' twice"); continue }
        $seenM.Add($id)
        if (-not [string]$m.title) { $bad.Add("model $id has no title") }
        if (-not [string]$m.file -or ([string]$m.file) -match '[\\/]') { $bad.Add("model $id file '$([string]$m.file)' is empty or a path") }
        if ((([string]$m.url) -notmatch '^https://')) { $bad.Add("model $id url is not https") }
        if (-not [string]$m.licence -or -not [string]$m.licence_url) { $bad.Add("model $id has no licence line") }
        if ([long]$m.size -le 0) { $bad.Add("model $id has no size") }
        if ((([string]$m.sha256) -notmatch '^[0-9A-Fa-f]{64}$')) { $bad.Add("model $id has no SHA-256 pin") }
    }
    return $bad.ToArray()
}

# Pure: why this component cannot be installed from this package, or $null when it can. A build that has not yet
# uploaded its asset leaves the pin unfilled; the component is then refused, never installed unchecked.
function Get-ComponentPinProblem($Component) {
    $a = $Component.asset
    if ($null -eq $a) { return 'the manifest names no release asset for it' }
    if ((([string]$a.sha256) -notmatch '^[0-9A-Fa-f]{64}$')) { return "its download has no SHA-256 in this package (components.json says '$([string]$a.sha256)')" }
    if ([long]$a.size -le 0) { return 'its download has no size in this package' }
    return $null
}

# Pure: the addresses of a component's asset, in manifest order.
function Get-ComponentAssetUrls($Manifest, $Component) {
    foreach ($t in @($Component.asset.url_templates)) {
        ([string]$t).Replace('{release}', [string]$Manifest.release).Replace('{name}', [string]$Component.asset.name)
    }
}

function Get-ComponentById($Manifest, [string]$Id) { foreach ($c in @($Manifest.components)) { if ([string]$c.id -eq $Id) { return $c } } return $null }
function Get-ComponentModelById($Manifest, [string]$Id) { foreach ($m in @($Manifest.models)) { if ([string]$m.id -eq $Id) { return $m } } return $null }

# Pure: a size a person reads, from bytes.
# Pure except for the environment: the component's own model store in the user profile, or $null. The install and
# the uninstall both run elevated in the account that started them, so this is that account's folder.
function Get-ComponentUserModelsDir($Component) {
    $t = [string]$Component.user_models_dir
    if (-not $t) { return $null }
    $p = [Environment]::ExpandEnvironmentVariables($t)
    if ($p -match '%') { return $null }           # a variable this account does not have: nothing to point at
    return $p
}

function Format-ComponentSize([long]$Bytes) {
    if ($Bytes -ge 1073741824) { return ('{0:N1} GB' -f ($Bytes / 1073741824.0)) }
    if ($Bytes -ge 1048576) { return ('{0:N0} MB' -f ($Bytes / 1048576.0)) }
    return ('{0:N0} kB' -f ($Bytes / 1024.0))
}

# ---------------------------------------------------------------------------------------------------------------
# What to install: the switches, then the questions.

# Pure: what the command line asked for.
#   -NoComponents            -> mode 'none'
#   -Components a,b          -> mode 'list', the ids given (unknown ones are reported, not installed)
#   neither                  -> mode 'ask' when the run may ask, else 'default' (every component whose default is
#                               true) when the caller takes the defaults without asking (prepare-offline does; a run
#                               that the setup window drives does not, because nothing there consented to them)
function Resolve-ComponentRequest {
    param($Manifest, [string[]]$Requested, [bool]$NoComponents, [bool]$CanAsk, [bool]$DefaultsWithoutAsking)
    $out = [pscustomobject]@{ mode = 'none'; ids = @(); unknown = @() }
    if ($null -eq $Manifest) { return $out }
    if ($NoComponents) { return $out }
    $known = @(@($Manifest.components) | ForEach-Object { [string]$_.id })
    if (@($Requested).Count) {
        $ids = New-Object System.Collections.Generic.List[string]
        foreach ($r in @($Requested)) {
            foreach ($one in @(([string]$r) -split '[,; ]')) {
                $t = $one.Trim().ToLowerInvariant()
                if (-not $t) { continue }
                if ($known -contains $t) { if (-not $ids.Contains($t)) { $ids.Add($t) } } else { $out.unknown += $t }
            }
        }
        $out.mode = 'list'
        $out.ids = $ids.ToArray()
        return $out
    }
    if ($CanAsk) { $out.mode = 'ask'; return $out }
    if ($DefaultsWithoutAsking) {
        $out.mode = 'default'
        $out.ids = @(@($Manifest.components) | Where-Object { [bool]$_.default } | ForEach-Object { [string]$_.id })
    }
    return $out
}

# Pure: the same for the models. The model question's default is none, so an unattended install takes no model.
function Resolve-ModelRequest {
    param($Manifest, [string[]]$Requested, [bool]$NoModels, [bool]$CanAsk, [string[]]$ComponentIds)
    $out = [pscustomobject]@{ mode = 'none'; ids = @(); unknown = @(); unusable = @() }
    if ($null -eq $Manifest) { return $out }
    # Only a model that one of the chosen components can use is offered or accepted. With no extra chosen nothing
    # is usable, and a -Models that was given anyway is reported as unusable rather than ignored in silence.
    $usable = New-Object System.Collections.Generic.List[string]
    foreach ($id in @($ComponentIds)) {
        $c = Get-ComponentById $Manifest $id
        if ($null -eq $c) { continue }
        foreach ($m in @($c.models)) { if (-not $usable.Contains([string]$m)) { $usable.Add([string]$m) } }
    }
    if ($NoModels) { return $out }
    if (@($Requested).Count) {
        $known = @(@($Manifest.models) | ForEach-Object { [string]$_.id })
        $ids = New-Object System.Collections.Generic.List[string]
        foreach ($r in @($Requested)) {
            foreach ($one in @(([string]$r) -split '[,; ]')) {
                $t = $one.Trim().ToLowerInvariant()
                if (-not $t) { continue }
                if ($usable.Contains($t)) { if (-not $ids.Contains($t)) { $ids.Add($t) } }
                elseif ($known -contains $t) { $out.unusable += $t }
                else { $out.unknown += $t }
            }
        }
        $out.mode = 'list'
        $out.ids = $ids.ToArray()
        return $out
    }
    if ($CanAsk -and $usable.Count) { $out.mode = 'ask' }
    return $out
}

# Asks a yes/no question with a default that Enter keeps. Never asks in a dry run, never asks when the setup window
# drives the run (it has no console: what its command line did not give is not consented), and takes the default when
# the console cannot be read (an unattended -NonInteractive run).
function Read-YesNo {
    param([Parameter(Mandatory)][string]$Question, [bool]$Default)
    $shown = 'yes/No'
    if ($Default) { $shown = 'Yes/no' }
    if ($script:GuiMode) { Write-Info "$Question -> not given by the setup window: no"; return $false }
    if ($script:DryRunMode) {
        $d = 'no'
        if ($Default) { $d = 'yes' }
        Write-Host "   [dry run] would ask: $Question [$shown]; continuing with the default ($d)" -ForegroundColor DarkYellow
        return $Default
    }
    for ($i = 0; $i -lt 3; $i++) {
        $reply = $null
        try { $reply = Read-Host "   $Question [$shown]" } catch { Write-Info "$Question -> the console cannot be read: the default is used"; return $Default }
        $t = ([string]$reply).Trim().ToLowerInvariant()
        Write-Log "   asked: $Question -> '$reply'"
        if (-not $t) { return $Default }
        if ($t -in @('y', 'yes')) { return $true }
        if ($t -in @('n', 'no')) { return $false }
        Write-Host '   Please answer yes or no, or press Enter for the default.'
    }
    return $Default
}

# The component questions. Returns the chosen ids. Prints the title, the size and the licence of each component.
function Read-ComponentChoice {
    param($Manifest, $Rows)
    $ids = New-Object System.Collections.Generic.List[string]
    foreach ($r in @($Rows)) {
        $c = $r.component
        Write-Host ''
        Write-Host "   $([string]$c.title) - $(Format-ComponentSize ([long]$c.asset.size)) download, licence $([string]$c.licence): $([string]$c.licence_url)"
        if ([string]$c.summary) { Write-Host "   $([string]$c.summary)" }
        if ($r.refused) { Write-Warn2 "$([string]$c.title) cannot be installed from this package: $($r.refused). It is not offered."; continue }
        if (Read-YesNo -Question ([string]$c.question) -Default ([bool]$c.default)) { $ids.Add([string]$c.id) }
    }
    return $ids.ToArray()
}

# Pure: what one model costs on the disk with the chosen extras, in plain words, or $null when there is nothing to
# add to its own size. A model is stored once per route: our llama.cpp build reads the GGUF file, and our Ollama
# build keeps its own copy in its own format, so a model chosen with both extras is on the disk twice.
function Get-ModelCostText($Model, $GgufComponents, $PullComponents) {
    $parts = @()
    $total = [long]0
    if (@($GgufComponents).Count) {
        $parts += "$(Format-ComponentSize ([long]$Model.size)) for $(@(@($GgufComponents) | ForEach-Object { [string]$_.title }) -join ' and ')"
        $total += [long]$Model.size
    }
    $pullers = @()
    if ([string]$Model.ollama_tag) { $pullers = @(@($PullComponents) | ForEach-Object { [string]$_.title }) }
    if ($pullers.Count) {
        $parts += "about as much again for $($pullers -join ' and '), which keeps its own copy in its own format"
        $total += [long]$Model.size
    }
    if ($parts.Count -lt 2) { return $null }
    return ("$($parts -join ', plus ') - about $(Format-ComponentSize $total) on the disk in all")
}

# The model question: one short list with sizes and licences, numbers or ids, Enter for none. $Components are the
# extras that were just chosen, so each line can say what the model costs with them.
function Read-ModelChoice {
    param($Manifest, [string[]]$Offered, $Components)
    $rows = @(foreach ($id in @($Offered)) { Get-ComponentModelById $Manifest $id })
    if (-not $rows.Count) { return @() }
    $gguf = @(@($Components) | Where-Object { (Get-ComponentModelRoute $_) -eq 'gguf' })
    $pull = @(@($Components) | Where-Object { (Get-ComponentModelRoute $_) -eq 'pull' })
    Write-Host ''
    Write-Host '   Models. A model is a large download and it is not needed to install anything else.'
    for ($i = 0; $i -lt $rows.Count; $i++) {
        $m = $rows[$i]
        $note = [string]$m.note
        if ([bool]$m.needs_12gb) { $note = ('needs the 12 GB board memory setting. ' + $note).Trim() }
        Write-Host ("   {0}) {1,-34} {2,9}  licence {3}: {4}" -f ($i + 1), [string]$m.title, (Format-ComponentSize ([long]$m.size)), [string]$m.licence, [string]$m.licence_url)
        $cost = Get-ModelCostText $m $gguf $pull
        if ($cost) { Write-Host ("      {0}" -f $cost) }
        if ($note) { Write-Host ("      {0}" -f $note) }
    }
    if ($script:GuiMode) { Write-Info 'models -> not given by the setup window: none'; return @() }
    if ($script:DryRunMode) {
        Write-Host '   [dry run] would ask which models to download; continuing with the default (none)' -ForegroundColor DarkYellow
        return @()
    }
    $reply = $null
    try { $reply = Read-Host '   Which models shall be downloaded? Numbers or names, separated by commas; Enter for none' }
    catch { Write-Info 'models -> the console cannot be read: none'; return @() }
    Write-Log "   asked: models -> '$reply'"
    $ids = New-Object System.Collections.Generic.List[string]
    foreach ($one in @(([string]$reply) -split '[,; ]')) {
        $t = $one.Trim().ToLowerInvariant()
        if (-not $t) { continue }
        $m = $null
        if ($t -match '^\d+$') { $n = [int]$t; if ($n -ge 1 -and $n -le $rows.Count) { $m = $rows[$n - 1] } }
        else { foreach ($r in $rows) { if ([string]$r.id -eq $t) { $m = $r } } }
        if ($null -eq $m) { Write-Warn2 "'$($one.Trim())' is not one of the models above: skipped"; continue }
        if (-not $ids.Contains([string]$m.id)) { $ids.Add([string]$m.id) }
    }
    return $ids.ToArray()
}

# The rows the questions and the install work from: one per component of the manifest, with the reason a component
# cannot be installed from this package. Pure.
function Get-ComponentRows($Manifest) {
    $out = @()
    foreach ($c in @($Manifest.components)) {
        $out += [pscustomobject]@{ id = [string]$c.id; component = $c; refused = (Get-ComponentPinProblem $c) }
    }
    return $out
}

# ---------------------------------------------------------------------------------------------------------------
# Getting the files. The component asset goes through the same route as the firmware: every address in manifest
# order, up to $Tries per address, the SHA-256 checked after every try, nothing kept that does not match.

function Get-ComponentAssetStaged {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)]$Component, [Parameter(Mandatory)][string]$Staging,
          [string]$FromDir, [int]$Tries = $script:ComponentTries, [int]$TimeoutSec = $script:ComponentTimeoutSec)
    $problem = Get-ComponentPinProblem $Component
    if ($problem) { throw "$([string]$Component.id): $problem" }
    [void][IO.Directory]::CreateDirectory($Staging)
    Enable-Tls12
    $name = [string]$Component.asset.name
    $want = ([string]$Component.asset.sha256).ToUpperInvariant()
    $dst = Join-Path $Staging $name
    if (Test-Path -LiteralPath $dst -PathType Leaf) {
        if ((Get-Sha256 $dst) -eq $want) { Write-Info "$name is already staged with the pinned SHA256"; return $dst }
        Remove-Item -LiteralPath $dst -Force
    }
    $why = @()
    if ($FromDir) {
        # A file that is in the folder decides the matter: it is used when it matches the pin and refused when it
        # does not. A file that is not in the folder at all is a miss, not a fault, so a folder prepared for one
        # extra can still be used by an install that asks for two: what is not there is downloaded instead.
        $src = Join-Path $FromDir $name
        if (Test-Path -LiteralPath $src -PathType Leaf) {
            [IO.File]::Copy($src, $dst, $true)
            $h = Get-Sha256 $dst
            if ($h -ne $want) { Remove-Item -LiteralPath $dst -Force; throw "$([string]$Component.id): $src has SHA256 $h, this release pins $want" }
            Write-Info ('{0,-40} SHA256 {1} ok  {2}' -f $name, $want, $src)
            return $dst
        }
        Write-Warn2 "$name is not in $FromDir`: it is downloaded instead"
        $why += "$FromDir`: $name is not there"
    }
    $oldProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        foreach ($u in Get-ComponentAssetUrls $Manifest $Component) {
            $host2 = ([Uri]$u).Host
            for ($i = 1; $i -le $Tries; $i++) {
                if ($script:NetworkBlocked) { $why += "$host2`: network unavailable (test)"; break }
                try { Invoke-WebRequest -Uri $u -OutFile $dst -UseBasicParsing -TimeoutSec $TimeoutSec -UserAgent $script:DownloadUserAgent }
                catch { $why += "$host2 try $i`: $($_.Exception.Message)"; continue }
                $h = Get-Sha256 $dst
                if ($h -eq $want) {
                    Write-Info ('{0,-40} SHA256 {1} ok  {2}' -f $name, $want, $u)
                    return $dst
                }
                $why += "$host2`: SHA256 $h"
                Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
                break
            }
        }
    } finally { $ProgressPreference = $oldProgress }
    throw "$([string]$Component.id): $name could not be downloaded with the pinned SHA256 $want ($($why -join '; '))"
}

# A large file (a model is gigabytes). curl.exe ships with Windows 11 and resumes a part file; without it the file
# comes down through .NET in 1 MB blocks. Returns the number of bytes in the finished file; throws on failure.
function Get-LargeFile {
    param([Parameter(Mandatory)][string]$Url, [Parameter(Mandatory)][string]$Destination, [int]$TimeoutSec = 0)
    if ($script:NetworkBlocked) { throw 'network unavailable (test)' }
    $part = $Destination + '.part'
    $curl = Join-Path $env:windir 'System32\curl.exe'
    if (Test-Path -LiteralPath $curl -PathType Leaf) {
        $args2 = @('--location', '--fail', '--retry', '3', '--retry-delay', '5', '--continue-at', '-', '--silent', '--show-error',
                   '--user-agent', $script:DownloadUserAgent, '--output', $part, $Url)
        $n = Invoke-Native $curl $args2
        if ($n.code -ne 0) { throw "curl.exe exit $($n.code): $($n.text)" }
    } else {
        Enable-Tls12
        $req = [Net.HttpWebRequest]::Create($Url)
        $req.UserAgent = $script:DownloadUserAgent
        if ($TimeoutSec -gt 0) { $req.Timeout = $TimeoutSec * 1000 }
        $resp = $req.GetResponse()
        try {
            $in = $resp.GetResponseStream()
            $out = [IO.File]::Create($part)
            try {
                $buf = New-Object byte[] 1048576
                while ($true) {
                    $r = $in.Read($buf, 0, $buf.Length)
                    if ($r -le 0) { break }
                    $out.Write($buf, 0, $r)
                }
            } finally { $out.Close(); $in.Close() }
        } finally { $resp.Close() }
    }
    if (Test-Path -LiteralPath $Destination -PathType Leaf) { Remove-Item -LiteralPath $Destination -Force }
    [IO.File]::Move($part, $Destination)
    return (Get-Item -LiteralPath $Destination).Length
}

# Pure except for reading the volume: the free bytes of the volume a path is on, or -1 when it cannot be read.
function Get-FreeBytes([string]$Path) {
    try {
        $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
        if (-not $root) { return -1 }
        return [long](New-Object IO.DriveInfo $root).AvailableFreeSpace
    } catch { return -1 }
}

# One model into $ModelsDir, SHA-256 checked. A file that is already there with the pinned SHA-256 is kept and
# nothing is downloaded. A file that is not in $FromDir is a miss, not a fault: it is downloaded instead, as the
# component assets are. Returns 'present', 'downloaded' or 'copied'; throws when the file cannot be had.
function Get-ModelFile {
    param([Parameter(Mandatory)]$Model, [Parameter(Mandatory)][string]$ModelsDir, [string]$FromDir)
    $want = ([string]$Model.sha256).ToUpperInvariant()
    $dst = Join-Path $ModelsDir ([string]$Model.file)
    [void][IO.Directory]::CreateDirectory($ModelsDir)
    if (Test-Path -LiteralPath $dst -PathType Leaf) {
        if ((Get-Item -LiteralPath $dst).Length -eq [long]$Model.size -and (Get-Sha256 $dst) -eq $want) { return 'present' }
        Write-Warn2 "$dst is not the file this release pins: it is replaced"
        Remove-Item -LiteralPath $dst -Force
    }
    $src = $null
    if ($FromDir) {
        $p = Join-Path $FromDir ([string]$Model.file)
        if (Test-Path -LiteralPath $p -PathType Leaf) { $src = $p }
        else { Write-Warn2 "$([string]$Model.file) is not in $FromDir`: it is downloaded instead" }
    }
    # A model is one to eleven gigabytes. An hour of downloading that ends in a full disk is worth one line first.
    $free = Get-FreeBytes $ModelsDir
    $need = [long]$Model.size + 268435456
    if ($free -ge 0 -and $free -lt $need) {
        throw ("$([string]$Model.file) needs $(Format-ComponentSize ([long]$Model.size)) and the disk of $ModelsDir " +
               "has $(Format-ComponentSize $free) free: it was not fetched. Make room, or download it later.")
    }
    if ($src) { [IO.File]::Copy($src, $dst, $true) }
    else { [void](Get-LargeFile -Url ([string]$Model.url) -Destination $dst) }
    $h = Get-Sha256 $dst
    if ($h -ne $want) {
        Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
        throw "$([string]$Model.file) has SHA256 $h, this release pins $want (the file was not kept)"
    }
    if ($src) { return 'copied' }
    return 'downloaded'
}

# ---------------------------------------------------------------------------------------------------------------
# Installing a component: unpack the checked zip, write the backend setting, register what section 5 of the
# component contract allows and nothing else.

# Pure: the names inside a zip that may not be unpacked (an absolute path, or one that leaves the folder).
function Get-ZipUnsafeEntries([string[]]$Names) {
    $bad = @()
    foreach ($n in @($Names)) {
        $t = ([string]$n).Replace('/', '\')
        if ($t -match '^[\\]|^[A-Za-z]:' -or $t -match '(^|\\)\.\.(\\|$)') { $bad += [string]$n }
    }
    return $bad
}

# Unpacks $Zip into $Destination (emptied of nothing: files are compared by SHA-256 and replaced as everywhere else).
# $ZipRoot names the one top-level directory whose contents are taken: a name, or '*' for "the one folder the zip
# holds, whatever it is called", which a build folder that carries a version or a commit needs. Empty takes the
# zip's own files. Returns the number of files in the component folder.
function Expand-ComponentZip {
    param([Parameter(Mandatory)][string]$Zip, [Parameter(Mandatory)][string]$Destination, [string]$ZipRoot, [Parameter(Mandatory)][string]$Staging)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $names = @()
    $z = [IO.Compression.ZipFile]::OpenRead($Zip)
    try { $names = @($z.Entries | ForEach-Object { $_.FullName }) } finally { $z.Dispose() }
    $unsafe = @(Get-ZipUnsafeEntries $names)
    if ($unsafe.Count) { throw "$Zip holds entries that would be written outside $Destination`: $($unsafe -join ', ')" }
    $tmp = Join-Path $Staging ('unpack-' + [IO.Path]::GetFileNameWithoutExtension($Zip))
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    [void][IO.Directory]::CreateDirectory($tmp)
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $tmp)
    $from = $tmp
    if ($ZipRoot -eq '*') {
        # The one folder the zip holds, whatever it is called: a build whose folder carries a version or a commit
        # needs no new name in components.json at every release.
        $tops = @(Get-ChildItem -LiteralPath $tmp -Force)
        if ($tops.Count -ne 1 -or -not $tops[0].PSIsContainer) {
            throw "$Zip does not hold exactly one folder, which components.json zip_root '*' asks for ($($tops.Count) item(s) at its top level)"
        }
        $from = $tops[0].FullName
    } elseif ($ZipRoot) {
        $from = Join-Path $tmp $ZipRoot
        if (-not (Test-Path -LiteralPath $from -PathType Container)) { throw "$Zip has no folder '$ZipRoot' (components.json zip_root)" }
    }
    [void](Copy-TreeSafe -Source $from -Destination $Destination)
    $n = @(Get-ChildItem -LiteralPath $Destination -Recurse -File).Count
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    return $n
}

# A Start menu or Startup shortcut. Returns the path it wrote.
function New-ComponentShortcut {
    param([Parameter(Mandatory)][string]$Link, [Parameter(Mandatory)][string]$Target, [string]$Arguments, [string]$WorkDir, [bool]$Hidden)
    [void][IO.Directory]::CreateDirectory((Split-Path $Link))
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($Link)
    $s.TargetPath = $Target
    if ($Arguments) { $s.Arguments = $Arguments }
    $w = $WorkDir
    if (-not $w) { $w = (Split-Path $Target) }
    $s.WorkingDirectory = $w
    if ($Hidden) { $s.WindowStyle = 7 }          # minimised: a server launcher does not take the screen
    $s.Save()
    return $Link
}

# Pure: the shortcut names of a component (the install writes them, the uninstaller removes them by these names).
function Get-ComponentShortcutPaths($Component) {
    $out = [pscustomobject]@{ start_menu = $null; startup = $null }
    $title = [string]$Component.title
    if ([string]$Component.entry) { $out.start_menu = Join-Path $script:StartMenuDir "amdgpu-wddm $title.lnk" }
    if ([string]$Component.server_launcher) { $out.startup = Join-Path $script:StartupDir "amdgpu-wddm $title server.lnk" }
    return $out
}

# Installs one checked component zip. Every change goes through Invoke-Change, so a dry run prints it and changes
# nothing. Returns the record that goes into the installer state.
function Install-ComponentFiles {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)]$Component, [Parameter(Mandatory)][string]$Zip,
          [Parameter(Mandatory)][string]$InstallRoot, [Parameter(Mandatory)][string]$Staging, [string]$ModelsDir)
    $id = [string]$Component.id
    $dir = Join-Path $InstallRoot ([string]$Component.install_dir)
    $zipRoot = [string]$Component.zip_root
    Invoke-Change "unpack $([IO.Path]::GetFileName($Zip)) ($([string]$Component.title), SHA256 checked) -> $dir" {
        [void](Expand-ComponentZip -Zip $Zip -Destination $dir -ZipRoot $zipRoot -Staging $Staging)
    } | Out-Null
    $record = [ordered]@{ id = $id; title = [string]$Component.title; version = [string]$Component.version; dir = $dir
        asset = [string]$Component.asset.name; asset_sha256 = ([string]$Component.asset.sha256).ToUpperInvariant()
        path_entry = $null; start_menu = $null; startup = $null; backend = $null; hooks = $null; models_dir = $ModelsDir
        user_models_dir = (Get-ComponentUserModelsDir $Component) }
    # The backend setting: one line in the component's own folder, which its launcher reads (component contract 6).
    $bs = $Component.backend_setting
    if ($null -ne $bs) {
        $file = Join-Path $dir ([string]$bs.file)
        $value = [string]$bs.default
        Invoke-Change "write $file`: $([string]$bs.key)=$value (the backend $([string]$Component.title) uses; change the line to one of $(@($bs.values) -join ', '))" {
            [void][IO.Directory]::CreateDirectory((Split-Path $file))
            [IO.File]::WriteAllText($file, "$([string]$bs.key)=$value`r`n", (New-Object Text.UTF8Encoding $false))
        } | Out-Null
        $record.backend = $value
    }
    # A component that registers something of its own does it here, through its own script (component contract 3).
    # It runs elevated, like the rest of the install: a component that writes a per-user setting writes it for the
    # account that started install.cmd (the same account after a UAC prompt), not for every account of the computer.
    if (Get-ComponentHookScript $Component $dir) {
        $r = Invoke-ComponentHook -Component $Component -Dir $dir -Verb 'register' -TimeoutSeconds 120 `
            -Description "register $([string]$Component.title) for this account (its logon task, its backend setting and its model folder)"
        if ($r.ran -and $r.code -ne 0) {
            Write-Warn2 "$([string]$Component.title) is installed, but its own registration ended with exit $($r.code). Its files are in $dir; start it from there, or run uninstall.cmd and install it again."
        }
        $record.hooks = [string]$Component.hooks.script
    }
    if ([bool]$Component.path_entry) {
        $entry = $dir
        if (Test-PathEntry (Get-MachinePath).value $entry) { Write-Info "the machine PATH holds $entry already: kept" }
        else {
            Invoke-Change "append $entry to the machine PATH (so $([string]$Component.title) can be started from any command prompt)" {
                $p = Get-MachinePath
                Set-MachinePath (Add-PathEntry $p.value $entry) $p.kind
            } | Out-Null
        }
        $record.path_entry = $entry
    }
    $links = Get-ComponentShortcutPaths $Component
    if ($links.start_menu) {
        $target = Join-Path $dir ([string]$Component.entry)
        Invoke-Change "Start menu shortcut $($links.start_menu) -> $target" {
            [void](New-ComponentShortcut -Link $links.start_menu -Target $target -WorkDir $dir -Hidden $false)
        } | Out-Null
        $record.start_menu = $links.start_menu
    }
    if ($links.startup) {
        $target = Join-Path $dir ([string]$Component.server_launcher)
        Invoke-Change "Startup shortcut $($links.startup) -> $target (starts the server for whoever logs on; no service, no administrator rights)" {
            [void](New-ComponentShortcut -Link $links.startup -Target $target -WorkDir $dir -Hidden $true)
        } | Out-Null
        $record.startup = $links.startup
    }
    return $record
}

# Pure: how a component gets a model. A component that has a 'pull' hook pulls the model by its registry tag
# through its own script; every other component reads GGUF files from the models folder.
function Get-ComponentModelRoute($Component) {
    if ($null -ne $Component.hooks -and $null -ne $Component.hooks.pull) { return 'pull' }
    return 'gguf'
}

# The chosen models, for the components that were installed. A model that cannot be had is a warning and nothing
# more: the driver and the components stay installed. Returns the records for the installer state.
function Install-ComponentModels {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)]$Components, [string[]]$ModelIds,
          [Parameter(Mandatory)][string]$InstallRoot, [Parameter(Mandatory)][string]$ModelsDir, [string]$FromDir)
    $out = @()
    if (-not @($ModelIds).Count) { return $out }
    $gguf = @(@($Components) | Where-Object { (Get-ComponentModelRoute $_) -eq 'gguf' })
    $pull = @(@($Components) | Where-Object { (Get-ComponentModelRoute $_) -eq 'pull' })
    foreach ($id in @($ModelIds)) {
        $m = Get-ComponentModelById $Manifest $id
        if ($null -eq $m) { Write-Warn2 "model '$id' is not offered by this release: skipped"; continue }
        # Each model is its own try: a model that cannot be had must not take the models after it with it.
        try {
            if ($gguf.Count) {
                $from = 'the Hugging Face address in this package'
                if ($FromDir) { $from = $FromDir }
                $state2 = Invoke-Change "get $([string]$m.file) ($(Format-ComponentSize ([long]$m.size)), $([string]$m.title)) from $from into $ModelsDir and check its SHA256" {
                    return (Get-ModelFile -Model $m -ModelsDir $ModelsDir -FromDir $FromDir)
                }
                if ($null -eq $state2) { $state2 = 'dry-run' }
                if ($state2 -eq 'present') { Write-Info "$([string]$m.file) was already there with the pinned SHA256: kept" }
                $out += [ordered]@{ id = $id; file = [string]$m.file; path = (Join-Path $ModelsDir ([string]$m.file)); sha256 = ([string]$m.sha256).ToUpperInvariant(); state = $state2 }
            }
        } catch {
            Write-Warn2 "$([string]$m.title) was not downloaded: $($_.Exception.Message). The other models and the extras are not affected."
        }
        if ($pull.Count -and [string]$m.ollama_tag) {
            foreach ($c in $pull) {
                # A model is gigabytes over the internet: the pull gets two hours, or what is left of the run's own
                # deadline, and a pull that does not finish is a warning. The component and the driver stay
                # installed either way.
                $r = Invoke-ComponentHook -Component $c -Dir (Join-Path $InstallRoot ([string]$c.install_dir)) -Verb 'pull' -Tag ([string]$m.ollama_tag) -TimeoutSeconds (Get-HookDeadlineSeconds 7200) `
                    -Description "pull $([string]$m.ollama_tag) ($([string]$m.title)) with $([string]$c.title)"
                if ($r.ran -and $r.code -ne 0) { Write-Warn2 "$([string]$c.title) could not pull $([string]$m.ollama_tag) (exit $($r.code)). Pull it later with: ollama pull $([string]$m.ollama_tag)" }
                elseif ($r.ran) { $out += [ordered]@{ id = $id; file = $null; path = $null; sha256 = $null; state = "pulled by $([string]$c.id) as $([string]$m.ollama_tag)" } }
            }
        }
    }
    return $out
}

# How long a hook may run: what it asks for, or what is left of the run's own deadline when that is less. A run
# without a deadline (install.cmd from a command prompt) gets what the caller asked for.
function Get-HookDeadlineSeconds([double]$Want) {
    if ($null -eq $script:EngineDeadline) { return $Want }
    $left = ($script:EngineDeadline - [DateTime]::UtcNow).TotalSeconds
    if ($left -le 0) { return 1 }
    if ($left -lt $Want) { return [double]$left }
    return $Want
}

# ---------------------------------------------------------------------------------------------------------------
# Hooks. A component that has to register something of its own (the Ollama component writes its logon task, its
# backend setting and its model folder itself: component contract sections 3 to 7) ships one .ps1 in its own folder
# and names its verbs in components.json. The installer runs that script and reads its exit code, never its prose,
# so a non-English Windows changes nothing (issue #35).

# Pure: the hook script of a component inside its installed folder, or $null when it has none.
function Get-ComponentHookScript($Component, [string]$Dir) {
    if ($null -eq $Component.hooks -or -not [string]$Component.hooks.script) { return $null }
    return (Join-Path $Dir ([string]$Component.hooks.script))
}
# Pure: the arguments of one verb, with {tag} replaced. $null when the component does not have that verb.
function Get-ComponentHookArguments($Component, [string]$Verb, [string]$Tag) {
    if ($null -eq $Component.hooks) { return $null }
    $a = $Component.hooks.$Verb
    if ($null -eq $a -or -not @($a).Count) { return $null }
    return @(@($a) | ForEach-Object { ([string]$_).Replace('{tag}', $Tag) })
}
# Runs one verb of a component's hook script under a deadline. Returns the exit code and the output; never throws.
# A dry run prints the command and runs nothing.
function Invoke-ComponentHook {
    param([Parameter(Mandatory)]$Component, [Parameter(Mandatory)][string]$Dir, [Parameter(Mandatory)][string]$Verb,
          [string]$Tag, [double]$TimeoutSeconds = 120, [string]$Description)
    $script2 = Get-ComponentHookScript $Component $Dir
    $args2 = Get-ComponentHookArguments $Component $Verb $Tag
    if (-not $script2 -or $null -eq $args2) { return [pscustomobject]@{ ran = $false; code = 0; text = "component $([string]$Component.id) has no '$Verb' hook" } }
    $ps = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $full = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script2) + $args2
    $what = $Description
    if (-not $what) { $what = "run $([string]$Component.hooks.script) $($args2 -join ' ') ($([string]$Component.title))" }
    $out = Invoke-Change $what {
        if (-not (Test-Path -LiteralPath $script2 -PathType Leaf)) { return [pscustomobject]@{ ran = $false; code = -1; text = "$script2 is not there" } }
        $n = Invoke-NativeBounded -File $ps -Arguments $full -TimeoutSeconds $TimeoutSeconds
        return [pscustomobject]@{ ran = $true; code = [int]$n.code; text = [string]$n.text; timed_out = [bool]$n.timed_out }
    }
    if ($null -eq $out) { return [pscustomobject]@{ ran = $false; code = 0; text = '(dry run)' } }
    if ($out.text) { Write-Log "   $Verb hook of $([string]$Component.id): $($out.text)" }
    return $out
}

# ---------------------------------------------------------------------------------------------------------------
# Uninstall and the footprint.

# Pure: the component records of the installer state, whatever an older state wrote there.
function Get-InstalledComponentRecords($State) {
    if ($null -eq $State) { return @() }
    $p = $State.PSObject.Properties['components_installed']
    if (-not $p -or $null -eq $p.Value) { return @() }
    return @($p.Value)
}
function Get-InstalledModelRecords($State) {
    if ($null -eq $State) { return @() }
    $p = $State.PSObject.Properties['models_installed']
    if (-not $p -or $null -eq $p.Value) { return @() }
    return @($p.Value)
}
# Pure: the models folder this installation used, from the state, else the default.
function Get-StateModelsDir($State) {
    foreach ($r in @(Get-InstalledComponentRecords $State)) { if ([string]$r.models_dir) { return [string]$r.models_dir } }
    if ($State -and $State.PSObject.Properties['models_dir'] -and [string]$State.models_dir) { return [string]$State.models_dir }
    return $script:ModelsDirDefault
}

# Each installed component undoes its own registration first, with its own script, while its folder is still there
# (component contract 7). A script that is gone, fails or passes its deadline is a warning: the PATH entries, the
# shortcuts and the folders below are removed by the uninstaller itself whatever the script did.
function Invoke-ComponentUnregister($State, $Manifest) {
    foreach ($r in @(Get-InstalledComponentRecords $State)) {
        if (-not [string]$r.hooks) { continue }
        $dir = [string]$r.dir
        $c = $null
        if ($null -ne $Manifest) { $c = Get-ComponentById $Manifest ([string]$r.id) }
        if ($null -eq $c) {
            # The package that uninstalls is not always the one that installed: the state's own record is enough to
            # call the script, so a component installed by an earlier release still unregisters itself.
            $c = [pscustomobject]@{ id = [string]$r.id; title = [string]$r.title; install_dir = $null
                hooks = [pscustomobject]@{ script = [string]$r.hooks; unregister = @('unregister') } }
        }
        $out = Invoke-ComponentHook -Component $c -Dir $dir -Verb 'unregister' -TimeoutSeconds 120 `
            -Description "let $([string]$r.title) remove its own registration ($([string]$r.hooks) unregister)"
        if ($out.ran -and $out.code -ne 0) {
            Write-Warn2 "$([string]$r.title) ended its own removal with exit $($out.code). Its files and shortcuts are removed below; a logon entry of its own may stay. Check Task Scheduler for an entry with BC250 in its name."
        } elseif (-not $out.ran) { Write-Info "$([string]$r.title): $($out.text)" }
    }
}

# Removes the registrations of every installed component: the PATH entries and the shortcuts. The component folders
# themselves go with the install root, which the uninstaller removes anyway.
function Remove-ComponentRegistrations($State) {
    $records = @(Get-InstalledComponentRecords $State)
    if (-not $records.Count) { Write-Info 'no component of ours is recorded in the installer state: nothing to unregister'; return }
    $entries = @(@($records | ForEach-Object { [string]$_.path_entry }) | Where-Object { $_ })
    $onPath = @($entries | Where-Object { Test-PathEntry (Get-MachinePath).value $_ })
    if ($onPath.Count) {
        Invoke-Change "remove $($onPath -join ', ') from the machine PATH (the component folders of this release)" {
            $p = Get-MachinePath
            $v = $p.value
            foreach ($e in $onPath) { $v = Remove-PathEntry $v $e }
            Set-MachinePath $v $p.kind
        } | Out-Null
    }
    foreach ($r in $records) {
        foreach ($lnk in @([string]$r.start_menu, [string]$r.startup)) {
            if (-not $lnk) { continue }
            if (-not (Test-Path -LiteralPath $lnk)) { Write-Info "already gone: $lnk"; continue }
            Invoke-Change "remove the shortcut $lnk ($([string]$r.title))" { Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue } | Out-Null
        }
    }
}

# The downloaded models. They are the user's data: kept unless the uninstaller runs with -RemoveModels.
function Remove-ComponentModels($State, [bool]$RemoveModels) {
    Remove-ComponentUserStores $State $RemoveModels
    $dir = Get-StateModelsDir $State
    if (-not (Test-Path -LiteralPath $dir)) { Write-Info "no models folder ($dir)"; return }
    $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue)
    $bytes = 0
    foreach ($f in $files) { $bytes += $f.Length }
    if (-not $RemoveModels) {
        Write-Info "kept: $dir ($($files.Count) file(s), $(Format-ComponentSize $bytes)); they are your models. Run uninstall.cmd -RemoveModels to delete them, or delete the folder yourself."
        return
    }
    Invoke-Change "remove $dir with the downloaded models ($($files.Count) file(s), $(Format-ComponentSize $bytes)): -RemoveModels was given" {
        Remove-PathOrSchedule $dir
        $parent = Split-Path $dir
        if ((Test-Path -LiteralPath $parent) -and -not @(Get-ChildItem -LiteralPath $parent -Force).Count) { Remove-Item -LiteralPath $parent -Force -ErrorAction SilentlyContinue }
    } | Out-Null
}

# A component that keeps the models in its own format has its own store in the user profile. -RemoveModels clears
# those stores as well, because the person asked for the models to go. Each one is a folder of the account that
# runs the uninstall: another account's copy stays, and the list at the end of the uninstall says so.
function Remove-ComponentUserStores($State, [bool]$RemoveModels) {
    foreach ($r in @(Get-InstalledComponentRecords $State)) {
        $dir = Get-StateUserModelsDir $r
        if (-not $dir) { continue }
        if (-not (Test-Path -LiteralPath $dir)) { Write-Info "no model store of $([string]$r.title) ($dir)"; continue }
        $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue)
        $bytes = 0
        foreach ($f in $files) { $bytes += $f.Length }
        if (-not $RemoveModels) {
            Write-Info "kept: $dir ($($files.Count) file(s), $(Format-ComponentSize $bytes)); the models of $([string]$r.title), in its own format."
            continue
        }
        Invoke-Change "remove $dir with the models of $([string]$r.title) ($($files.Count) file(s), $(Format-ComponentSize $bytes)): -RemoveModels was given" {
            Remove-PathOrSchedule $dir
        } | Out-Null
    }
}

# Pure: a component's own model store as the install recorded it, or $null. An older state has no such field.
function Get-StateUserModelsDir($Record) {
    if ($null -eq $Record) { return $null }
    $t = [string]$Record.user_models_dir          # '' for a state that has no such field, and for a hashtable record
    if (-not $t) { return $null }
    return $t
}

# Footprint rows for the uninstaller's "what is left" list. Each row is read, never changed; a probe that cannot
# read says so instead of claiming the item is gone.
function Get-ComponentFootprint($State, [bool]$RemoveModels) {
    $rows = New-Object System.Collections.ArrayList
    $add = {
        param([string]$Item, [scriptblock]$Read, [bool]$Kept)
        try { $r = & $Read; [void]$rows.Add([pscustomobject]@{ item = $Item; present = [bool]$r.present; detail = [string]$r.detail; kept = $Kept; known = $true }) }
        catch { [void]$rows.Add([pscustomobject]@{ item = $Item; present = $true; detail = "could not be read: $($_.Exception.Message)"; kept = $false; known = $false }) }
    }
    $records = @(Get-InstalledComponentRecords $State)
    & $add 'component folders' {
        $left = @(@($records | ForEach-Object { [string]$_.dir }) | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
        $d = 'no component folder of ours'
        if ($left.Count) { $d = ($left -join ', ') }
        elseif ($records.Count) { $d = "$($records.Count) component folder(s) of ours are gone" }
        @{ present = ($left.Count -gt 0); detail = $d }
    } $false
    & $add 'component PATH entries' {
        $entries = @(@($records | ForEach-Object { [string]$_.path_entry }) | Where-Object { $_ })
        $left = @($entries | Where-Object { Test-PathEntry (Get-MachinePath).value $_ })
        $d = 'no component folder of ours on the machine PATH'
        if ($left.Count) { $d = ($left -join ', ') }
        @{ present = ($left.Count -gt 0); detail = $d }
    } $false
    & $add 'component shortcuts' {
        $links = @(@($records | ForEach-Object { [string]$_.start_menu; [string]$_.startup }) | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
        $d = 'no shortcut of a component of ours'
        if ($links.Count) { $d = (@($links | ForEach-Object { Split-Path $_ -Leaf }) -join ', ') }
        @{ present = ($links.Count -gt 0); detail = $d }
    } $false
    # A component registers its own logon entry through its own script (component contract 3). The names of those
    # entries start with 'BC250 ', so this list can see one that a failed unregister left behind.
    & $add 'component logon entries' {
        $left = @()
        try { $left = @(Get-ScheduledTask -ErrorAction Stop | Where-Object { $_.TaskName -like $script:ComponentTaskPattern } | ForEach-Object { $_.TaskName }) }
        catch { throw }
        $d = "no scheduled task named $($script:ComponentTaskPattern)"
        if ($left.Count) { $d = ($left -join ', ') }
        @{ present = ($left.Count -gt 0); detail = $d }
    } $false
    $stores = @(@($records | ForEach-Object { Get-StateUserModelsDir $_ }) | Where-Object { $_ })
    if ($stores.Count) {
        & $add 'models of a component in its own format' {
            $left = @($stores | Where-Object { Test-Path -LiteralPath $_ })
            $d = (@($stores | ForEach-Object { "$_ is gone" }) -join ', ')
            if ($left.Count) {
                $parts = @(foreach ($s in $left) {
                    $files = @(Get-ChildItem -LiteralPath $s -Recurse -File -ErrorAction SilentlyContinue)
                    $bytes = 0
                    foreach ($f in $files) { $bytes += $f.Length }
                    "$s ($($files.Count) file(s), $(Format-ComponentSize $bytes))"
                })
                $d = ($parts -join ', ')
                if (-not $RemoveModels) { $d = "$d, your models" }
            }
            @{ present = ($left.Count -gt 0); detail = $d }
        } (-not $RemoveModels)
    }
    $modelsDir = Get-StateModelsDir $State
    & $add 'downloaded models' {
        $p = (Test-Path -LiteralPath $modelsDir)
        $d = "$modelsDir is gone"
        if ($p) {
            $files = @(Get-ChildItem -LiteralPath $modelsDir -Recurse -File -ErrorAction SilentlyContinue)
            $bytes = 0
            foreach ($f in $files) { $bytes += $f.Length }
            $d = "$modelsDir ($($files.Count) file(s), $(Format-ComponentSize $bytes))"
            if (-not $RemoveModels) { $d = "$d, your models" }
        }
        @{ present = $p; detail = $d }
    } (-not $RemoveModels)
    return $rows.ToArray()
}

# ---------------------------------------------------------------------------------------------------------------
# A prepared offline folder, and an install that reads one.

# Pure: where a prepared folder (or a kept repair set) keeps the component assets and the model files.
function Get-SetComponentDir([string]$SetRoot) { return (Join-Path $SetRoot $script:ComponentSetDir) }
function Get-SetModelDir([string]$SetRoot) { return (Join-Path $SetRoot $script:ModelSetDir) }

# The component folder of this package (a prepared offline folder or a repair set), or $null when there is none.
function Get-PackageComponentDir([string]$PackageRoot) {
    $d = Get-SetComponentDir $PackageRoot
    if ((Test-Path -LiteralPath $d -PathType Container) -and @(Get-ChildItem -LiteralPath $d -File).Count) { return $d }
    return $null
}
function Get-PackageModelDir([string]$PackageRoot) {
    $d = Get-SetModelDir $PackageRoot
    if ((Test-Path -LiteralPath $d -PathType Container) -and @(Get-ChildItem -LiteralPath $d -File).Count) { return $d }
    return $null
}

# Where the component zips of an offline source are: a prepared folder's own components\ subfolder when the folder
# given is the prepared root, else the folder itself (zips and model files together, put there by hand).
function Get-ComponentSourceDir([string]$Dir) {
    if (-not $Dir) { return $null }
    $sub = Get-SetComponentDir $Dir
    if (Test-Path -LiteralPath $sub -PathType Container) { return $sub }
    return $Dir
}

# Where the model files of an offline source are, given the folder the component zips come from. The component
# assets and the model files are not in the same folder in a prepared folder: prepare-offline.ps1 writes
# components\ and models\ side by side. A folder put together by hand may hold both together, which the lab's
# -ComponentDir route does, so both spellings work:
#   <d>\models exists                        -> <d>\models     a prepared folder, or a folder with a models\ in it
#   <d> is the components\ of a prepared set -> <d>\..\models  the install resolved the package's own folder
#   neither                                  -> <d>            the zips and the model files together
function Get-ModelSourceDir([string]$Dir) {
    if (-not $Dir) { return $null }
    $sub = Get-SetModelDir $Dir
    if (Test-Path -LiteralPath $sub -PathType Container) { return $sub }
    if ((Split-Path $Dir -Leaf) -eq $script:ComponentSetDir) {
        $sibling = Get-SetModelDir (Split-Path $Dir)
        if (Test-Path -LiteralPath $sibling -PathType Container) { return $sibling }
    }
    return $Dir
}

# What a folder is missing against the pins: one line per missing or changed file. Pure except for reading the files.
function Test-ComponentSetDir($Manifest, [string[]]$ComponentIds, [string]$Dir) {
    $bad = @()
    foreach ($id in @($ComponentIds)) {
        $c = Get-ComponentById $Manifest $id
        if ($null -eq $c) { $bad += "$id is not a component of this release"; continue }
        $problem = Get-ComponentPinProblem $c
        if ($problem) { $bad += "$id`: $problem"; continue }
        $p = Join-Path $Dir ([string]$c.asset.name)
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $bad += "missing $([string]$c.asset.name)"; continue }
        $h = Get-Sha256 $p
        if ($h -ne ([string]$c.asset.sha256).ToUpperInvariant()) { $bad += "changed $([string]$c.asset.name) (SHA256 $h)" }
    }
    return $bad
}
function Test-ModelSetDir($Manifest, [string[]]$ModelIds, [string]$Dir) {
    $bad = @()
    foreach ($id in @($ModelIds)) {
        $m = Get-ComponentModelById $Manifest $id
        if ($null -eq $m) { $bad += "$id is not a model of this release"; continue }
        $p = Join-Path $Dir ([string]$m.file)
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $bad += "missing $([string]$m.file)"; continue }
        $h = Get-Sha256 $p
        if ($h -ne ([string]$m.sha256).ToUpperInvariant()) { $bad += "changed $([string]$m.file) (SHA256 $h)" }
    }
    return $bad
}
