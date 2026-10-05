# Present heartbeat for the owner's session on unit A: a small borderless top-most window whose text changes
# every 250 ms for $Seconds, so DWM presents while transitions and trial admissions wait for a fresh KMD
# start-health witness (age_ms <= 15000). An idle desktop presents about once a minute. Runs as the interactive
# user through a one-shot scheduled task (heartbeat-start.ps1); exits by itself.
param([int]$Seconds = 150)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$f = New-Object Windows.Forms.Form
$f.FormBorderStyle = 'None'; $f.TopMost = $true; $f.ShowInTaskbar = $false; $f.StartPosition = 'Manual'
$f.Size = New-Object Drawing.Size(230, 22); $f.Location = New-Object Drawing.Point(8, 8)
$f.BackColor = [Drawing.Color]::Black
$l = New-Object Windows.Forms.Label
$l.Dock = 'Fill'; $l.ForeColor = [Drawing.Color]::Lime; $l.Font = New-Object Drawing.Font('Consolas', 9)
$f.Controls.Add($l)
$end = (Get-Date).AddSeconds($Seconds)
$t = New-Object Windows.Forms.Timer; $t.Interval = 250
$t.Add_Tick({ $n = Get-Date; if ($n -ge $end) { $f.Close() } else { $l.Text = 'bc250 heartbeat ' + $n.ToString('HH:mm:ss.f') } })
$t.Start()
[void]$f.ShowDialog()
