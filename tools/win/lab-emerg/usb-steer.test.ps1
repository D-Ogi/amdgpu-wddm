# Host test of the stick steering of listener.ps1, through its own Run-Action 'usb-boot'.
# No stick and no lab: the functions are taken from listener.ps1 through the parser (the listener itself is not
# started), Usb-Stick is replaced by one that names a directory tree under -Work with the stick's file layout.
# It also checks that install.ps1 stops before a change when a script file is missing in -Source.
# Usage: usb-steer.test.ps1 -Work <new empty directory>
param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\stick-env.ps1"
$utf8 = New-Object Text.UTF8Encoding($false)
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile("$PSScriptRoot\listener.ps1", [ref]$null, [ref]$errors)
if ($errors.Count) { throw "listener.ps1 has $($errors.Count) parse errors" }
foreach ($name in 'Usb-Signed', 'Usb-Default', 'Usb-SetSignedDefault', 'Usb-State', 'Run-Action') {
    $f = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true)
    if (-not $f) { throw "listener.ps1 has no function $name" }
    . ([scriptblock]::Create($f.Extent.Text))
}
$script:fail = 0
function Check([string]$What, [bool]$Ok) { if ($Ok) { "  PASS $What" } else { "  FAIL $What"; $script:fail++ } }
function Sha([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function Refused([scriptblock]$Do) { try { & $Do | Out-Null; return $false } catch { return $true } }

if (Get-ChildItem -LiteralPath $Work -Force) { throw '-Work must be an empty directory' }
function New-Tree([string]$Name) {
    $root = Join-Path $Work $Name
    New-Item -ItemType Directory -Force (Join-Path $root 'boot\grub'), (Join-Path $root 'efi\boot') | Out-Null
    $t = [ordered]@{ root = $root; grub = Join-Path $root 'boot\grub\grub.cfg'; efi = Join-Path $root 'efi\boot\bootx64.efi'
        off = Join-Path $root 'efi\boot\bootx64.off'; env = Join-Path $root 'boot\grub\grubenv' }
    [IO.File]::WriteAllText($t.grub, "set timeout=5`nset default=6`nmenuentry `"a`" {`n}`n")
    [IO.File]::WriteAllBytes($t.env, (Format-StickEnv ([ordered]@{ bc250_try = '1' })))
    [IO.File]::WriteAllText($t.efi, 'loader')
    return $t
}

'-- the stick of today (no grub.cfg.sig)'
$script:tree = New-Tree 'old'
function Usb-Stick { $script:tree }
$envHash = Sha $tree.env
$s = Run-Action 'usb-boot' 'linux:4' $null
Check 'linux:4 edits set default in grub.cfg' ($s.default -eq '4' -and -not $s.signed)
Check 'linux:4 keeps grub.cfg.orig' (Test-Path -LiteralPath ($tree.grub + '.orig'))
$s = Run-Action 'usb-boot' 'linux:6' $null
Check 'linux:6 sets default 6 (Windows through X3)' ($s.default -eq '6')
Check 'grubenv is not touched on this stick' ((Sha $tree.env) -eq $envHash)
$s = Run-Action 'usb-boot' 'windows' $null
Check 'windows switches the loader off' ((Test-Path $tree.off) -and -not (Test-Path $tree.efi))
$s = Run-Action 'usb-boot' 'linux:4' $null
Check 'linux:4 switches the loader on again' ((Test-Path $tree.efi) -and -not (Test-Path $tree.off))

'-- the stick with a signed menu (grub.cfg.sig)'
$script:tree = New-Tree 'signed'
[IO.File]::WriteAllText($tree.grub + '.sig', 'x')
$cfgHash = Sha $tree.grub
$s = Run-Action 'usb-boot' 'status' $null
Check 'status: signed, default 6 with no variable' ($s.signed -and $s.default -eq '6')
$s = Run-Action 'usb-boot' 'linux:4' $null
Check 'linux:4 writes bc250_default into grubenv' ($s.default -eq '4')
Check 'the X3 counter line survives' ((Read-StickEnv ([IO.File]::ReadAllBytes($tree.env)))['bc250_try'] -eq '1')
Check 'the first block is kept as grubenv.orig' (Test-Path -LiteralPath ($tree.env + '.orig'))
Check 'linux:7 is refused' (Refused { Run-Action 'usb-boot' 'linux:7' $null })
Check 'a refused entry leaves the old value' ((Run-Action 'usb-boot' 'status' $null).default -eq '4')
$s = Run-Action 'usb-boot' 'windows' $null
Check 'windows switches the loader off' ((Test-Path $tree.off) -and -not (Test-Path $tree.efi))
$s = Run-Action 'usb-boot' 'linux:6' $null
Check 'linux:6 with the loader off: variable removed, loader on' ($s.default -eq '6' -and (Test-Path $tree.efi) -and -not (Test-Path $tree.off))
Check 'the block equals grubenv.orig again' ((Sha $tree.env) -eq (Sha ($tree.env + '.orig')))
$null = Run-Action 'usb-boot' 'windows' $null
$s = Run-Action 'usb-boot' 'linux:2' $null
Check 'linux:2 with the loader off: value written, loader on' ($s.default -eq '2' -and (Test-Path $tree.efi))
Check 'grub.cfg is never changed and has no .orig copy' ((Sha $tree.grub) -eq $cfgHash -and -not (Test-Path -LiteralPath ($tree.grub + '.orig')))
Check 'the block is 1024 bytes' ((Get-Item -LiteralPath $tree.env).Length -eq 1024)

'-- install.ps1 with a missing script file'
$src = Join-Path $Work 'src'
New-Item -ItemType Directory -Force $src | Out-Null
Copy-Item "$PSScriptRoot\listener.ps1" $src
$ErrorActionPreference = 'Continue'   # Windows PowerShell 5.1 turns a child's error text into an error record
$out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\install.ps1" -Source $src 2>&1 | Out-String
Check 'install stops when stick-env.ps1 is missing' ($LASTEXITCODE -ne 0 -and $out -match 'stick-env\.ps1 is not in' -and $out -match 'nothing was changed')
Remove-Item (Join-Path $src 'listener.ps1'); Copy-Item "$PSScriptRoot\stick-env.ps1" $src
$out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\install.ps1" -Source $src 2>&1 | Out-String
Check 'install stops when listener.ps1 is missing' ($LASTEXITCODE -ne 0 -and $out -match 'listener\.ps1 is not in')

if ($script:fail) { "USB-STEER TEST: $($script:fail) FAIL"; exit 1 }
'USB-STEER TEST: ALL PASS'
