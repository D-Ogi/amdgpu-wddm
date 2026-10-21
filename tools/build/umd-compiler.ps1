# Compiler-explicit user-mode DLL builds. Host MSVC ABI/static-analysis controls remain separate.
function Resolve-Bc250UmdCompiler([string]$Root,[string]$Compiler,[string]$MsvcReason,[string]$Out,[string]$Arch) {
    if($Compiler -eq 'msvc') {
        if([string]::IsNullOrWhiteSpace($MsvcReason)){throw '-Compiler msvc requires a concrete -MsvcReason'}
        $path=(Get-Command cl.exe -ErrorAction Stop).Source
        $version=(& $path 2>&1 | Out-String).Trim()
        Write-Host "Explicit MSVC exception: $MsvcReason"
    } else {
        $path=Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
        if(!(Test-Path -LiteralPath $path)){throw "Required clang-cl is missing: $path"}
        $version=(& $path --version | Out-String).Trim()
        if($LASTEXITCODE -ne 0 -or $version -notmatch 'clang version 23\.1\.4\b'){throw 'Expected clang-cl 23.1.4'}
    }
    Write-Host "User-mode compiler: $path"
    Write-Host $version
    @{compiler=$Compiler;path=$path;version=$version;arch=$Arch;msvc_reason=$MsvcReason} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Out 'compiler.json') -Encoding utf8
    return $path
}

function ConvertTo-Bc250ClangFlags([string[]]$Flags,[string]$Arch,[string[]]$Roots) {
    $result=@($Flags | Where-Object { $_ -ne '/GL' -and $_ -ne '/FC' -and $_ -notlike '/d1trimfile:*' })
    $target=if($Arch -eq 'x86'){'i686-pc-windows-msvc'}else{'x86_64-pc-windows-msvc'}
    $result+=@("--target=$target",'/FIwindows.h','/FIwinternl.h','/clang:-march=x86-64-v3','/clang:-mtune=znver2')
    # Vulkan's {sType} aggregate idiom value-initializes every remaining field to zero.
    $result+='/clang:-Wno-missing-field-initializers'
    foreach($sourceRoot in $Roots){$result+="/clang:-ffile-prefix-map=$sourceRoot=."}
    # LLVM native COFF objects link with the Windows tools, including existing MSVC /GL dependency libraries.
    return $result
}
