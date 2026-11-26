# rapidcheck-lib.ps1 - builds the vendored RapidCheck library for the property gates, and caches the result.
#
# Dot-source it and call Build-Bc250RapidCheck. The property test runners of two components use it
# (driver\contract\test\run_record_properties.ps1 and driver\kmd\test\run_flip_state_property.ps1), so the library
# is built in one place and with one set of flags.
#
# The library is third-party C++11 of 2015 (third_party\rapidcheck\PROVENANCE.md). It compiles with /W3 and without
# /WX, because its warnings are not ours to fix, and a /W4 /WX run of it would make an upstream update a day of
# edits to a directory the provenance says we never edit. Our own test sources keep /W4 /WX.
#
# The cache is a stamp file beside the library: the source list, the compiler and the flags. An unchanged stamp
# skips the whole build, which is what makes the gates cheap enough to run on every quick.ps1.

function Get-Bc250RapidCheckSources([string]$Repo) {
    $src = Join-Path $Repo 'third_party\rapidcheck\src'
    if (-not (Test-Path -LiteralPath $src)) { throw "the vendored RapidCheck is not in this tree: $src" }
    return @(Get-ChildItem -LiteralPath $src -Recurse -Filter '*.cpp' -File | Sort-Object FullName)
}

function Get-Bc250RapidCheckInclude([string]$Repo) {
    return (Join-Path $Repo 'third_party\rapidcheck\include')
}

# Compiles the library into -Out and returns the object files to link. $Compiler and $Flags are the caller's own
# host-test compiler and base flags: the library must be built by the same compiler, for the same architecture and
# with the same runtime library as the test that links it.
function Build-Bc250RapidCheck([string]$Repo, [string]$CompilerExe, [string[]]$BaseFlags, [string]$Out) {
    $sources = Get-Bc250RapidCheckSources $Repo
    $include = Get-Bc250RapidCheckInclude $Repo
    New-Item -ItemType Directory -Force $Out | Out-Null
    $Out = (Resolve-Path -LiteralPath $Out).Path
    # /W3 and no /WX for the library; the caller's /W4 /WX is removed from the inherited flags.
    $flags = @($BaseFlags | Where-Object { $_ -ne '/W4' -and $_ -ne '/WX' -and $_ -ne '/W3' })
    # NOMINMAX is a requirement of the library, not a preference: the host-test flags force-include a header that
    # reaches windows.h, whose min and max macros break every std::numeric_limits<T>::max() in RapidCheck. The
    # caller states it too, for its own sources; it is stated here because this is where the requirement is known.
    $flags += @('/W3', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$include",
                '/clang:-Wno-deprecated-declarations', '/clang:-Wno-unused-parameter')

    $stampPath = Join-Path $Out 'rapidcheck.stamp'
    $identity = [pscustomobject]@{
        compiler = $CompilerExe
        flags    = ($flags -join ' ')
        sources  = @($sources | ForEach-Object { '{0} {1}' -f $_.FullName, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash })
    } | ConvertTo-Json -Depth 4
    $objects = @($sources | ForEach-Object { Join-Path $Out ([IO.Path]::GetFileNameWithoutExtension($_.Name) + '.obj') })
    if ((Test-Path -LiteralPath $stampPath) -and
        ((Get-Content -LiteralPath $stampPath -Raw) -ceq $identity) -and
        -not @($objects | Where-Object { -not (Test-Path -LiteralPath $_) })) {
        Write-Host "rapidcheck: $($objects.Count) objects from cache"
        return $objects
    }

    # Two source files of the library share a base name (detail\Platform.cpp and gen\Numeric.cpp are unique, but a
    # future revision may not be), so a collision is refused here instead of silently overwriting one object.
    $collisions = @($objects | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
    if ($collisions) { throw "RapidCheck sources share an object name: $($collisions -join ', ')" }

    Write-Host "rapidcheck: building $($sources.Count) sources"
    for ($i = 0; $i -lt $sources.Count; $i++) {
        # Out-Host, not the pipeline: this function returns the object list, and a warning line left in the
        # pipeline would be returned as one more object file.
        Invoke-Bc250UmdCompiler $CompilerExe @flags /c "/Fo$($objects[$i])" $sources[$i].FullName | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "RapidCheck compilation failed: $($sources[$i].Name)" }
    }
    Set-Content -LiteralPath $stampPath -Value $identity -NoNewline
    return $objects
}
