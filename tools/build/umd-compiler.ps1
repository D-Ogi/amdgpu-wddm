# Compiler-explicit user-mode DLL builds. Host MSVC ABI/static-analysis controls remain separate.
# -MsvcPath names the cl.exe of a recipe that does not load the Visual Studio environment.
function Resolve-Bc250UmdCompiler([string]$Root,[string]$Compiler,[string]$MsvcReason,[string]$Out,[string]$Arch,[string]$MsvcPath='') {
    if($Compiler -eq 'msvc') {
        if([string]::IsNullOrWhiteSpace($MsvcReason)){throw '-Compiler msvc requires a concrete -MsvcReason'}
        $path=if($MsvcPath){$MsvcPath}else{(Get-Command cl.exe -ErrorAction Stop).Source}
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
    $result+=@("--target=$target","/FI$PSScriptRoot\clang-windows.h",'/clang:-march=x86-64-v3','/clang:-mtune=znver2',
        '/clang:-Werror=ignored-attributes','/clang:-Werror=ignored-qualifiers')
    # Mark SDK/CRT roots as system headers. /W4 must still diagnose project code;
    # LLVM otherwise emits MSVC-only SDK diagnostics under the recipe's /WX.
    $result+=@(($env:INCLUDE -split ';' | Where-Object { $_ }) |
        ForEach-Object { "/imsvc$([IO.Path]::GetFullPath($_))" })
    $result+='/clang:-Wno-system-headers'
    # Vulkan's {sType} aggregate idiom value-initializes every remaining field to zero.
    $result+='/clang:-Wno-missing-field-initializers'
    foreach($sourceRoot in $Roots){$result+="/clang:-ffile-prefix-map=$sourceRoot=."}
    # LLVM native COFF objects link with the Windows tools, including existing MSVC /GL dependency libraries.
    return $result
}

function Invoke-Bc250UmdCompiler {
    # _CL_ is appended after /link by clang-cl and reaches the linker as input
    # files. Pass the Clang policy explicitly; leave MSVC's own controls intact.
    $exe=$args[0]
    $compilerArguments=@($args | Select-Object -Skip 1)
    $previousCl=$env:CL
    $previousTrailingCl=$env:_CL_
    try {
        if([IO.Path]::GetFileName($exe) -ieq 'clang-cl.exe'){$env:CL=$null;$env:_CL_=$null}
        & $exe @compilerArguments
        $global:LASTEXITCODE=$LASTEXITCODE
    } finally {$env:CL=$previousCl;$env:_CL_=$previousTrailingCl}
}

# Standalone SDK recipes pass explicit header roots and can invoke the linker in
# the same command. Keep compiler flags before /link and leave linker inputs intact.
function Invoke-Bc250SdkCompiler {
    param([string]$Exe,[string[]]$Arguments,[string]$Arch='x64',[string[]]$Roots=@())
    $converted=@($Arguments)
    if([IO.Path]::GetFileName($Exe) -ieq 'clang-cl.exe') {
        $cut=[Array]::IndexOf($converted,'/link')
        $compile=if($cut -ge 0){@($converted | Select-Object -First $cut)}else{$converted}
        $tail=if($cut -ge 0){@($converted | Select-Object -Skip $cut)}else{@()}
        $compile=@($compile | ForEach-Object {
            if($_ -cmatch '^/I' -and ($_ -match '[\\/]MSVC[\\/]|[\\/]microsoft\.windows\.sdk\.cpp[\\/]')) {
                '/imsvc'+$_.Substring(2)
            } elseif($_ -notin @('/we4229','/we4440')) { $_ }
        })
        $converted=@(ConvertTo-Bc250ClangFlags -Flags $compile -Arch $Arch -Roots $Roots)+$tail
    }
    Invoke-Bc250UmdCompiler $Exe @converted
    $global:LASTEXITCODE=$LASTEXITCODE
}

function Test-Bc250ClangSdk([string]$CompilerExe,[string[]]$Flags,[string]$Out) {
    $probe=Join-Path $PSScriptRoot 'umd-compiler-sdk-test.cpp'
    $obj=Join-Path $Out 'clang-sdk-probe.obj'
    Invoke-Bc250UmdCompiler $CompilerExe @Flags /c "/Fo$obj" $probe
    if($LASTEXITCODE){throw 'Clang SDK probe compile failed'}
    $exe=Join-Path $Out 'clang-sdk-test.exe'
    $includes=@($Flags | Where-Object { $_ -like '/external:I*' -or $_ -cmatch '^/I' })
    & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /external:W0 @includes /DBC250_COMPILER_CALLER /DNOMINMAX "/Fe$exe" $probe $obj
    if($LASTEXITCODE){throw 'MSVC/Clang SDK boundary control build failed'}
    & $exe
    if($LASTEXITCODE){throw 'MSVC/Clang SDK boundary control failed'}
    $asm=& dumpbin.exe /nologo /disasm $obj | Out-String
    $asm | Set-Content -LiteralPath (Join-Path $Out 'clang-sdk-probe.disasm.txt')
    foreach($mnemonic in @('prefetcht0','prefetcht1','prefetcht2')) {
        if($asm -notmatch "\b$mnemonic\b"){throw "Clang intrinsic control lacks $mnemonic"}
    }
    Write-Host 'PASS: Clang prefetch T0/T1/T2 instruction encodings'
    $negative=Join-Path $Out 'clang-sdk-bad-pack.cpp'
    "#pragma pack(push,1)`nstruct BadPacking { char byte; unsigned long long wide; };" |
        Set-Content -LiteralPath $negative
    $rejected=Invoke-Bc250UmdCompiler $CompilerExe @Flags /c "/Fo$(Join-Path $Out 'clang-sdk-bad-pack.obj')" $negative 2>&1 | Out-String
    $rejected | Set-Content -LiteralPath (Join-Path $Out 'clang-sdk-bad-pack.log')
    if($LASTEXITCODE -eq 0 -or $rejected -notmatch '-Wpragma-pack') {
        throw 'Project packing negative control did not fail for the expected diagnostic'
    }
    Write-Host 'PASS: project packing warnings remain fatal'
    $negativeAttribute=Join-Path $Out 'clang-sdk-bad-attribute.cpp'
    'int __stdcall invalid_data_calling_convention;' | Set-Content -LiteralPath $negativeAttribute
    $rejected=Invoke-Bc250UmdCompiler $CompilerExe @Flags /c "/Fo$(Join-Path $Out 'clang-sdk-bad-attribute.obj')" $negativeAttribute 2>&1 | Out-String
    $rejected | Set-Content -LiteralPath (Join-Path $Out 'clang-sdk-bad-attribute.log')
    if($LASTEXITCODE -eq 0 -or $rejected -notmatch '-Wignored-attributes') {
        throw 'Project calling-convention negative control did not fail for the expected diagnostic'
    }
    Write-Host 'PASS: project calling-convention warnings remain fatal'
}
