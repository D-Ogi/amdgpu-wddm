# Host-only argument boundary controls. No compiler or GPU work is started.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'umd-compiler.ps1')
function Invoke-Bc250UmdCompiler {
    $script:captured = @($args)
    $global:LASTEXITCODE = 19
}
$inputArgs = @('/c','/GL','/FC','/we4229','/we4440','/IP:\kits\microsoft.windows.sdk.cpp\c\Include\um',
    '/IP:\project\include','/link','/DEBUG','/LIBPATH:P:\lib','kernel32.lib')
Invoke-Bc250SdkCompiler -Exe 'clang-cl.exe' -Arguments $inputArgs -Arch x64 -Roots @('P:\project')
if ($LASTEXITCODE -ne 19) { throw 'Compiler failure exit was lost' }
$separator = [Array]::IndexOf($script:captured,'/link')
$tail = @($script:captured | Select-Object -Skip $separator)
if (($tail -join '|') -ne '/link|/DEBUG|/LIBPATH:P:\lib|kernel32.lib') { throw 'Linker tail changed' }
foreach($flag in @('/GL','/FC','/we4229','/we4440')) {
    if($flag -in $script:captured) { throw "MSVC-only compiler flag remains: $flag" }
}
foreach($flag in @('/IP:\project\include','/imsvcP:\kits\microsoft.windows.sdk.cpp\c\Include\um',
    '--target=x86_64-pc-windows-msvc','/clang:-Werror=ignored-attributes')) {
    $at=[Array]::IndexOf($script:captured,$flag)
    if($at -lt 0 -or $at -ge $separator) { throw "Compile flag missing or sent to linker: $flag" }
}
Invoke-Bc250SdkCompiler -Exe 'cl.exe' -Arguments $inputArgs
if(($script:captured -join '|') -ne ((@('cl.exe')+$inputArgs) -join '|')) { throw 'MSVC arguments changed' }
Invoke-Bc250SdkCompiler -Exe 'clang-cl.exe' -Arguments @('/c','file.cpp') -Arch x86
if('--target=i686-pc-windows-msvc' -notin $script:captured -or '/link' -in $script:captured) {
    throw 'Compile-only x86 invocation changed'
}
'PASS: standalone SDK compiler boundaries, SDK includes, architectures and exit propagation'
