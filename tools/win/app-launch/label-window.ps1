# A window shaped like the start window of tester.31 rc4 (start-window.ps1 before 84f35234: a borderless navy form at
# 8,34 with one control, Dock Fill, cyan Consolas 9, two lines, a countdown that changes once a second, a 250 ms
# timer), with the text drawn the way Nagi Control App draws its labels: visual styles on and TextRenderer
# (EnableVisualStyles, SetCompatibleTextRenderingDefault false, as Program.Main sets them). rc4 drew with GDI+ instead,
# because a PowerShell host makes no such call. -Kind picks the control that draws the text:
#   Label   a WinForms Label (a STATIC window)
#   Steady  SteadyLabel, a Label subclass in the WinForms window class, loaded from -App. No release of the control
#           application has it: its source is evidence/windows/2026-10-14-start-window-flash/scripts/SteadyLabel-run5.cs,
#           and -App must be a build of the control application with that file added.
# The window has the docked Label with AutoSize off of M898 on purpose: it shows the partial picture at each change of
# the text. The gate that refuses that layout in the repository's scripts allows this script by name.
# measure-flash.ps1 runs it as case c, and flash-watch reads DWM's frames and the window's own surface:
#
#   python tools\win\target.py ps tools\win\app-launch\measure-flash.ps1 -Kit C:\BC250\tmp\flash-kit -Fixed C:\BC250\tmp\flash-kit\label-window.ps1 -Cases c -CaseSeconds 60 -Surface -FixedKind Steady -FixedApp C:\BC250\tmp\flash-kit\amdgpu_wddm_control.exe
#
#   -Check  make the window and its control without showing them, print the control's type, its window class and
#           its text rendering, and exit 0: a check on any computer, nothing on the screen
#
# Exit codes: 0 shown and closed (or -Check), 2 the window or the control could not be made.
param(
    [int]$Seconds = 60,
    [ValidateSet('Label', 'Steady')][string]$Kind = 'Label',
    [string]$App = (Join-Path $PSScriptRoot 'amdgpu_wddm_control.exe'),
    [switch]$Check
)
$ErrorActionPreference = 'Stop'

# The text of rc4's start window, character for character.
function Format-StartWindowText([double]$Left) {
    $s = [int][Math]::Max(0, [Math]::Ceiling($Left))
    return ("Nagi start check: confirming that the GPU driver started correctly.`r`n" +
        'Nothing to do. This window closes by itself in {0}:{1:00}.') -f [int][Math]::Floor($s / 60), ($s % 60)
}

try {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
    [Windows.Forms.Application]::SetCompatibleTextRenderingDefault($false)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $form = New-Object Windows.Forms.Form
    $form.FormBorderStyle = 'None'; $form.TopMost = $true; $form.ShowInTaskbar = $false; $form.ShowIcon = $false
    $form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point(8, 34)
    $form.BackColor = [Drawing.Color]::Navy
    if ($Kind -eq 'Steady') {
        $type = [Reflection.Assembly]::LoadFrom($App).GetType('AmdgpuWddmControl.SteadyLabel', $true)
        $label = [Activator]::CreateInstance($type, $true)
    } else {
        $label = New-Object Windows.Forms.Label
    }
    $label.Dock = 'Fill'; $label.ForeColor = [Drawing.Color]::Cyan; $label.Font = New-Object Drawing.Font('Consolas', 9)
    $label.Padding = New-Object Windows.Forms.Padding(4, 3, 4, 3)
    $label.Text = Format-StartWindowText $Seconds
    $need = [Windows.Forms.TextRenderer]::MeasureText($label.Text, $label.Font)
    $form.ClientSize = New-Object Drawing.Size(($need.Width + 12), ($need.Height + 8))
    $form.Controls.Add($label)
    if ($Check) {
        Add-Type -Namespace LabelWindowCheck -Name Native -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)] public static extern int GetClassName(System.IntPtr hwnd, System.Text.StringBuilder name, int size);'
        $null = $form.Handle; $handle = $label.Handle
        $name = New-Object Text.StringBuilder 256
        [void][LabelWindowCheck.Native]::GetClassName($handle, $name, $name.Capacity)
        "kind $Kind, control $($label.GetType().FullName), window class $name, compatible text rendering $($label.UseCompatibleTextRendering), client $($form.ClientSize.Width)x$($form.ClientSize.Height), shown $($form.Visible)"
        $form.Dispose()
        exit 0
    }
    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 250
    $timer.Add_Tick({
        $label.Text = Format-StartWindowText ($Seconds - $clock.Elapsed.TotalSeconds)
        if ($clock.Elapsed.TotalSeconds -ge $Seconds) { $timer.Stop(); $form.Close() }
    })
    $timer.Start()
    [void]$form.ShowDialog()
    $timer.Dispose(); $form.Dispose()
} catch {
    exit 2
}
exit 0
