# Build-time metadata from the control application's pure catalog. No Windows settings are read.
function New-SystemTuningManifest([string]$CoreModule) {
    Import-Module -Name $CoreModule -Force -Scope Local
    $recommended = @(Get-TuningCatalog -Scope Machine | Where-Object recommended)
    return [ordered]@{
        selected_by_default = $false
        recommended = @($recommended | ForEach-Object id)
        recommended_items = @($recommended | ForEach-Object { [ordered]@{ id=$_.id; label=$_.label; note=$_.note } })
        source = 'tools/win/system-tuning'
        recovery_state = 'ProgramData/amdgpu-wddm/system-tuning'
        preserve_recovery_state = $true
    }
}
