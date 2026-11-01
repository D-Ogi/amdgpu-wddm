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

# BD-268: what a clang-cl build adds so that its PDB does not depend on the directories of the build. Under /Brepro the
# PDB GUID and the PE timestamp are a hash of the PDB, so a path of the build in the PDB gives another image for every
# output directory or tree. The caller compiles and links in its output directory with paths relative to it (objects
# with /Fo, the image, the import library, a .def file): without /Fo, clang-cl writes each object to a temporary file
# with a random name, and the PDB names it. Compile: the compile directory is recorded as "." and the cc1 command line,
# which names the tree and the output directory, is not recorded. Link: lld-link makes a relative path absolute against
# a fixed base instead of the current directory. test-pdb-repro.ps1 is the host test.
function Get-Bc250ReproducibleFlags([string]$Compiler) {
    if ($Compiler -ne 'clang-cl') { return [pscustomobject]@{ Compile = @(); Link = @() } }
    return [pscustomobject]@{ Compile = @('/clang:-ffile-compilation-dir=.', '/clang:-gno-codeview-command-line')
                              Link = @('/pdbsourcepath:X:\bc250-build') }
}

# Host test builds (tools/build/test-umd-*.ps1): the cl flags of a test plus the header roots that it reads as
# system headers. clang-cl gets the shell conversion and /imsvc for each root; cl gets /external:I and /external:W0.
function Get-Bc250HostTestFlags([string]$Compiler,[string[]]$Flags,[string[]]$Headers,[string]$Arch,[string[]]$Roots) {
    $Headers=@($Headers | Where-Object { $_ })
    if($Compiler -eq 'clang-cl') {
        return @(ConvertTo-Bc250ClangFlags $Flags $Arch $Roots)+@($Headers | ForEach-Object { "/imsvc$_" })
    }
    if($Headers.Count){return @($Flags)+@('/external:W0')+@($Headers | ForEach-Object { "/external:I$_" })}
    return @($Flags)
}

# Select the CRT/tool installation independently of the user-mode compiler. An explicit -VsInstall wins over
# BC250_VS_INSTALL; otherwise Find-VsInstall requires the x86/x64 C++ component in vswhere. Invalid explicit paths
# fail instead of selecting another installation. Use that installation's default VC toolset, not a lexical sort.
function Resolve-Bc250Toolchain([string]$VsInstall='', [string]$Arch='x64') {
    . "$PSScriptRoot\common.ps1"
    if (-not $VsInstall) { $VsInstall = $env:BC250_VS_INSTALL }
    $vs = Find-VsInstall $VsInstall
    $versionFile = Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid VC toolset version in $versionFile" }
    $msvc = Join-Path $vs "VC\Tools\MSVC\$version"
    $cl = Join-Path $msvc "bin\Hostx64\$Arch\cl.exe"
    foreach ($required in @($cl, "$msvc\bin\Hostx64\$Arch\link.exe", "$msvc\include\vcruntime.h", "$msvc\lib\$Arch\libcmt.lib")) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required VC toolchain file is missing: $required" }
    }
    Write-Host "Visual Studio tools: $vs (VC $version, $Arch)"
    return [pscustomobject]@{ VsInstall=$vs; Msvc=$msvc; Cl=$cl }
}

# Native tools and their host tests over the SDK NuGet packages under -Kits and the selected Visual Studio CRT.
# INCLUDE, LIB and PATH name only those, so neither clang-cl nor cl adds the headers or libraries of an
# installed SDK, and lld-link and link find the import libraries through LIB. Returns the Visual Studio tools folder
# and its cl.exe for -Arch. The caller puts the three variables back when it runs inside another build's process.
function Set-Bc250ToolEnvironment([string]$Kits,[string]$Arch='x64',[string]$KitVersion='10.0.26100.0',[string]$VsInstall='') {
    $tool=Resolve-Bc250Toolchain $VsInstall $Arch
    $msvc=$tool.Msvc
    $sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
    $sdkLib=Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
    $env:INCLUDE=@("$msvc\include","$sdk\Include\$KitVersion\ucrt","$sdk\Include\$KitVersion\um","$sdk\Include\$KitVersion\shared") -join ';'
    $env:LIB=@("$msvc\lib\$Arch","$sdkLib\ucrt\$Arch","$sdkLib\um\$Arch") -join ';'
    $env:PATH="$msvc\bin\Hostx64\$Arch;$env:PATH"
    return $tool
}

# The compile switches of a native tool in the form of the selected compiler, after Set-Bc250ToolEnvironment.
# clang-cl: the shell conversion (x86-64-v3, the INCLUDE roots as system headers, /FC and /d1trimfile removed in favour
# of -ffile-prefix-map over -Roots), the BD-268 compile switches and lld-link. cl: the switches as given; it reads
# INCLUDE itself.
function Get-Bc250ToolFlags([string]$Compiler,[string[]]$Flags,[string]$Arch,[string[]]$Roots) {
    if($Compiler -ne 'clang-cl'){return @($Flags)}
    return @(ConvertTo-Bc250ClangFlags $Flags $Arch $Roots)+@((Get-Bc250ReproducibleFlags 'clang-cl').Compile)+@('/clang:-fuse-ld=lld')
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
    # The MSVC object goes to $Out too: without /Fo, cl.exe writes it into the current directory, which the parallel
    # rows of a release set share (BD-266).
    $callerObj=Join-Path $Out 'umd-compiler-sdk-test.obj'
    & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /external:W0 @includes /DBC250_COMPILER_CALLER /DNOMINMAX "/Fo$callerObj" "/Fe$exe" $probe $obj
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
