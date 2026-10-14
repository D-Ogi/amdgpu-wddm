# Produce the unsigned container; EV signing and submission are separate acts.
# No certificate, private key, registry or device operation here.
function New-DriverSubmissionCab {
    param([Parameter(Mandatory)][string]$DriverDirectory, [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$KmdPdb, [Parameter(Mandatory)][string]$PdbReader)
    $source = [IO.Path]::GetFullPath($DriverDirectory).TrimEnd('\')
    $out = [IO.Path]::GetFullPath($Destination)
    $symbols = [IO.Path]::GetFullPath($KmdPdb)
    foreach ($path in $source,$out,$symbols,$PdbReader) {
        if ($path -notmatch '^[A-Za-z]:\\' -or $path.StartsWith('\\')) { throw 'submission paths must be local, not UNC paths' }
    }
    if ([IO.Path]::GetExtension($out) -ine '.cab' -or $out.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'submission CAB must be outside the driver package and have the .cab extension'
    }
    if (Test-Path -LiteralPath $out) { throw 'submission CAB already exists; use a new output name' }
    $pdbCheck = Invoke-Headless -File (Get-Command python).Source -Arguments @(
        (Join-Path $PSScriptRoot 'check_pdb.py'), '--image', (Join-Path $source 'bc250kmd.sys'),
        '--pdb', $symbols, '--reader', $PdbReader) -TimeoutSeconds 90
    if ($pdbCheck.code -ne 0) { throw $pdbCheck.text }
    foreach ($required in 'bc250kmd.inf','bc250kmd.cat','bc250kmd.sys','amdgpu-state.bin') {
        if (-not (Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) { throw "incomplete submission: $required" }
    }
    $entries = @(Get-ChildItem -LiteralPath $source -Recurse -Force)
    if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'submission contains a reparse point' }
    $files = @($entries | Where-Object { -not $_.PSIsContainer })
    $relative = @($files | ForEach-Object { $_.FullName.Substring($source.Length + 1) })
    if (@($files | Where-Object { $_.Extension -notin '.inf','.cat','.sys','.dll','.exe','.json','.config','.bin' }).Count) {
        throw 'submission driver folder contains a non-driver file'
    }
    $firmwareNames = @(Get-DriverStoreFirmwareNames)
    foreach ($required in $firmwareNames) { if ($relative -notcontains $required) { throw "submission lacks pinned firmware $required" } }
    Test-DriverCatalogMembers $source @($relative | Where-Object { $_ -ine 'bc250kmd.cat' })
    $parent = Split-Path $out
    [void][IO.Directory]::CreateDirectory($parent)
    $work = Join-Path $parent ('cab-work-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($work)
    try {
        if ($source.Contains('"') -or $work.Contains('"') -or $source.Contains("`n")) { throw 'invalid CAB path' }
        $lines = @('.OPTION EXPLICIT', '.Set Cabinet=ON', '.Set Compress=ON', '.Set CompressionType=MSZIP',
            '.Set MaxDiskSize=0', '.Set CabinetNameTemplate=driver.cab', ".Set DiskDirectoryTemplate=`"$work`"",
            ".Set RptFileName=`"$work\build.rpt`"", ".Set InfFileName=`"$work\build.inf`""
        )
        foreach ($r in $relative | Sort-Object) {
            $safe = [AmdgpuWddm.PackageStage]::MemberPath($r)
            $lines += ('"{0}" "driver\{1}"' -f (Join-Path $source $safe), $safe)
        }
        $lines += ('"{0}" "driver\bc250kmd.pdb"' -f $symbols)
        $ddf = Join-Path $work 'package.ddf'
        [IO.File]::WriteAllLines($ddf, $lines, [Text.Encoding]::Default)
        $makecab = Join-Path $env:windir 'System32\makecab.exe'
        $result = Invoke-Headless -File $makecab -Arguments @('/F',$ddf) -TimeoutSeconds 180
        if ($result.code -ne 0) { throw "submission CAB failed: $($result.text)" }
        $cab = Join-Path $work 'driver.cab'
        if (-not (Test-Path -LiteralPath $cab -PathType Leaf)) { throw 'makecab produced no CAB' }
        $expanded = Join-Path $work 'expanded'
        [void][IO.Directory]::CreateDirectory($expanded)
        $expand = Join-Path $env:windir 'System32\expand.exe'
        $result = Invoke-Headless -File $expand -Arguments @('-F:*',$cab,$expanded) -TimeoutSeconds 180
        if ($result.code -ne 0) { throw "submission CAB extraction check failed: $($result.text)" }
        $inside = Join-Path $expanded 'driver'
        $actual = @(Get-ChildItem -LiteralPath $expanded -Recurse -File)
        if ($actual.Count -ne $relative.Count + 1) { throw 'submission CAB member count changed' }
        if ((Get-FileHash -LiteralPath (Join-Path $inside 'bc250kmd.pdb')).Hash -cne (Get-FileHash -LiteralPath $symbols).Hash) {
            throw 'submission CAB changed the matching PDB'
        }
        foreach ($r in $relative) {
            $p = Join-Path $inside $r
            if (-not (Test-Path -LiteralPath $p -PathType Leaf) -or
                (Get-FileHash -LiteralPath $p).Hash -cne (Get-FileHash -LiteralPath (Join-Path $source $r)).Hash) {
                throw "submission CAB did not preserve $r"
            }
        }
        Test-DriverCatalogMembers $inside @($relative | Where-Object { $_ -ine 'bc250kmd.cat' })
        [IO.File]::Move($cab, $out)
        Write-Host "Unsigned Partner Center CAB: $out ($($files.Count) driver files including the catalog, plus matching PDB). Sign the CAB with EV before submission."
    } finally {
        # This operation owns only a fresh child of the chosen output directory.
        $resolvedWork = [IO.Path]::GetFullPath($work)
        if (-not $resolvedWork.StartsWith($parent.TrimEnd('\') + '\cab-work-', [StringComparison]::OrdinalIgnoreCase)) { throw 'CAB cleanup path escaped its output directory' }
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
