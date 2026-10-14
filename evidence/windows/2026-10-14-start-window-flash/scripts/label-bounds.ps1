# Host test, no window on the screen: the window positions (WM_WINDOWPOSCHANGING) that a text change gives a WinForms
# Label, for the layout of the start window of tester.31 rc4 (Dock Fill, AutoSize off) and for the layouts of the
# live labels of Nagi Control App (Ui.Setup: AutoSize on, a maximum width, in a FlowLayoutPanel or a TableLayoutPanel
# cell). Windows PowerShell 5.1:
#
#   powershell -NoProfile -File label-bounds.ps1
#
# The forms and labels get window handles and are never shown. A subclass of the Label's window (NativeWindow) notes
# the place, size and SWP_ flags of each WM_WINDOWPOSCHANGING while the script sets the Label's Text.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows.Forms;
namespace LabelBounds {
    public sealed class PosTrace : NativeWindow {
        public readonly List<string> Sizes = new List<string>();
        [StructLayout(LayoutKind.Sequential)]
        struct WindowPos { public IntPtr Hwnd, After; public int X, Y, Cx, Cy; public uint Flags; }
        public PosTrace(Control c) { AssignHandle(c.Handle); }
        protected override void WndProc(ref Message m) {
            if (m.Msg == 0x0046) {
                var p = (WindowPos)Marshal.PtrToStructure(m.LParam, typeof(WindowPos));
                Sizes.Add(p.X + "," + p.Y + " " + p.Cx + "x" + p.Cy + " flags 0x" + p.Flags.ToString("X4"));
            }
            base.WndProc(ref m);
        }
    }
}
'@

function Test-Case([string]$Name, [scriptblock]$Build, [string[]]$Texts) {
    $form = New-Object Windows.Forms.Form
    $form.ClientSize = New-Object Drawing.Size(400, 120)
    $label = & $Build $form
    $null = $form.Handle; $null = $label.Handle
    $form.PerformLayout()
    $trace = New-Object LabelBounds.PosTrace($label)
    foreach ($t in $Texts) {
        $trace.Sizes.Clear()
        $before = $label.Bounds
        $label.Text = $t
        "{0,-34} '{1}': {2} -> {3}; positions on the way: {4}" -f $Name, $t, "$($before.X),$($before.Y) $($before.Width)x$($before.Height)",
            "$($label.Left),$($label.Top) $($label.Width)x$($label.Height)", $(if ($trace.Sizes.Count) { $trace.Sizes -join ' | ' } else { 'none' })
    }
    $trace.ReleaseHandle(); $form.Dispose()
}

$texts = '61 C', '62 C', '100 C', 'A longer text that wraps inside the maximum width of the label, with more words than fit.'
Test-Case 'Dock Fill, AutoSize off (rc4)' { param($f) $l = New-Object Windows.Forms.Label; $l.Dock = 'Fill'; $l.Text = '60 C'; $f.Controls.Add($l); $l } $texts
Test-Case 'AutoSize, MaximumSize, flow' {
    param($f)
    $p = New-Object Windows.Forms.FlowLayoutPanel; $p.Dock = 'Fill'; $p.FlowDirection = 'TopDown'; $p.WrapContents = $false
    $l = New-Object Windows.Forms.Label; $l.AutoSize = $true; $l.MaximumSize = New-Object Drawing.Size(250, 0); $l.Text = '60 C'
    $p.Controls.Add($l); $f.Controls.Add($p); $l
} $texts
Test-Case 'AutoSize, MaximumSize, table cell' {
    param($f)
    $p = New-Object Windows.Forms.TableLayoutPanel; $p.Dock = 'Fill'; $p.ColumnCount = 2; $p.AutoSize = $true
    $k = New-Object Windows.Forms.Label; $k.AutoSize = $true; $k.Text = 'Temperature'
    $l = New-Object Windows.Forms.Label; $l.AutoSize = $true; $l.MaximumSize = New-Object Drawing.Size(250, 0); $l.Text = '60 C'
    $p.Controls.Add($k, 0, 0); $p.Controls.Add($l, 1, 0); $f.Controls.Add($p); $l
} $texts
