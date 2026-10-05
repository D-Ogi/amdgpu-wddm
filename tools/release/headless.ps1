# Invoke-Headless: runs a program with no window (CreateNoWindow), stdin closed, stdout and stderr captured, and a
# time bound. build-release.ps1 and test-dryrun.ps1 start every child process through it, so that nothing they run
# can show a console window, a prompt or a form on the development PC, and nothing can wait on input.
function Invoke-Headless {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @(), [int]$TimeoutSeconds = 300)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    foreach ($a in $Arguments) { [void]$psi.ArgumentList.Add($a) }
    # A Windows PowerShell 5.1 child started from PowerShell 7 inherits PSModulePath with the 7.x module folders and
    # then fails to load its own cmdlets; give it the machine and user values, as from Explorer or cmd.exe.
    $psi.Environment['PSModulePath'] = (@([Environment]::GetEnvironmentVariable('PSModulePath', 'Machine'), [Environment]::GetEnvironmentVariable('PSModulePath', 'User')) | Where-Object { $_ }) -join ';'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $p.StandardInput.Close()
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        try { $p.Kill($true) } catch { }
        throw "timeout after $TimeoutSeconds s: $File (pid $($p.Id))"
    }
    $p.WaitForExit()
    return @{ code = $p.ExitCode; text = ($so.Result + $se.Result); pid = $p.Id }
}
