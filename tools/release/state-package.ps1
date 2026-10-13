# Compare the package's immutable state bytes with its compiled CLI contract.
# The CLI catalog operation reads no installed driver, service or registry.
function Test-DriverStatePackage {
    param([Parameter(Mandatory)][string]$Package,
          [Parameter(Mandatory)][string]$CatalogSource,
          [scriptblock]$Runner)
    $bin = Join-Path $Package 'payload\kmd\amdgpu-state.bin'
    $worker = Join-Path $Package 'payload\tools\amdgpu-state-installer.exe'
    $cli = Join-Path $Package 'payload\tools\bc250kmd_cli.exe'
    foreach ($file in @($bin,$worker,$cli)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "missing state package component: $file" }
    }
    if ((Get-FileHash -LiteralPath $bin -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $CatalogSource -Algorithm SHA256).Hash) {
        throw 'packaged state catalog differs from generated source'
    }
    $bytes = [IO.File]::ReadAllBytes($bin)
    if ($bytes.Length -lt 80 -or [Text.Encoding]::ASCII.GetString($bytes,0,8) -cne "AMDST02`0" -or
        [BitConverter]::ToUInt32($bytes,8) -ne 2) { throw 'invalid state catalog header' }
    $count = [BitConverter]::ToUInt32($bytes,12)
    if ($count -gt 4096 -or $bytes.Length -ne (80 + [long]$count*12)) { throw 'invalid state catalog size' }
    $id = [Text.Encoding]::ASCII.GetString($bytes,16,64)
    if ($id -cnotmatch '^[0-9a-f]{64}$') { throw 'invalid state catalog identity' }
    if (-not $Runner) {
        $Runner = {param($exe,$arguments)
            $old=$ErrorActionPreference; $ErrorActionPreference='Continue'
            try {$text=(& $exe @arguments 2>&1 | ForEach-Object {[string]$_}) -join "`n"; $code=$LASTEXITCODE}
            finally {$ErrorActionPreference=$old}
            return @{code=$code;text=$text}
        }
    }
    $response = & $Runner $cli @('state','catalog')
    if ($response.code -ne 0) { throw "compiled state catalog failed: $($response.text)" }
    $compiled = $response.text | ConvertFrom-Json -ErrorAction Stop
    if ($compiled.abi -ne 1 -or $compiled.schema -ne 2 -or $null -eq $compiled.error -or
        $compiled.error -ne 0 -or $compiled.catalog_id -cne $id -or @($compiled.items).Count -ne $count) {
        throw 'compiled CLI and package state catalog differ'
    }
    # --catalog returns before SCM is opened. Do not run a service action here.
    $response = & $Runner $worker @('--catalog')
    if ($response.code -ne 0) { throw "compiled installer catalog failed: $($response.text)" }
    $compiled = $response.text | ConvertFrom-Json -ErrorAction Stop
    if ($compiled.abi -ne 1 -or $compiled.schema -ne 2 -or $null -eq $compiled.error -or
        $compiled.error -ne 0 -or $compiled.catalog_id -cne $id -or $compiled.count -ne $count) {
        throw 'compiled installer and package state catalog differ'
    }
    return "state package: schema 2, $count values, catalog $id"
}
