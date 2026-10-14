# Run 1's overlay bitmaps on the lab: what darkened the overlay's rectangle when the start window's PowerShell
# started and ended (the console question). Numbers only, and one cropped picture of the change. Reads only <Dir>,
# writes only <Out>. Windows PowerShell 5.1.
param([string]$Dir = 'C:\BC250\tmp\flash\20261014-092052', [string]$Out = 'C:\BC250\tmp\flash\run1-console.png',
      [int]$Threshold = 6)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class OverlayDiff {
    public static byte[] Luma(string path, out int w, out int h) {
        using (var b = new Bitmap(path)) {
            w = b.Width; h = b.Height;
            var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var raw = new byte[d.Stride * h];
            Marshal.Copy(d.Scan0, raw, 0, raw.Length);
            b.UnlockBits(d);
            var l = new byte[w * h];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int i = y * d.Stride + x * 4;
                    l[y * w + x] = (byte)((raw[i + 2] * 299 + raw[i + 1] * 587 + raw[i] * 114) / 1000);
                }
            return l;
        }
    }
    public static void Max(byte[] acc, byte[] l) { for (int i = 0; i < acc.Length; i++) if (l[i] > acc[i]) acc[i] = l[i]; }
    // Pixels at least t darker than the brightest value any frame had there: count and box (x0, y0, x1, y1).
    public static int[] Darker(byte[] max, byte[] l, int w, int h, int t) {
        int n = 0, x0 = w, y0 = h, x1 = -1, y1 = -1; long sum = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int d = max[y * w + x] - l[y * w + x];
                if (d >= t) { n++; sum += d; if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
            }
        return new[] { n, x0, y0, x1, y1, n > 0 ? (int)(sum / n) : 0 };
    }
    // Two differences side by side at half size, dark on white: a frame of the first phase and one of the second.
    public static void SavePair(byte[] max, byte[] a, byte[] b, int w, int h, string path) {
        int hw = w / 2, hh = h / 2;
        using (var bmp = new Bitmap(hw * 2 + 8, hh, PixelFormat.Format24bppRgb)) {
            for (int y = 0; y < hh; y++)
                for (int x = 0; x < hw * 2 + 8; x++) bmp.SetPixel(x, y, Color.FromArgb(255, 200, 200));
            for (int k = 0; k < 2; k++) {
                var l = k == 0 ? a : b;
                for (int y = 0; y < hh; y++)
                    for (int x = 0; x < hw; x++) {
                        int i = (y * 2) * w + x * 2;
                        int v = 255 - Math.Min(255, Math.Max(0, (max[i] - l[i]) * 8));
                        bmp.SetPixel(k * (hw + 8) + x, y, Color.FromArgb(v, v, v));
                    }
            }
            bmp.Save(path, ImageFormat.Png);
        }
    }
    // The change inside a box, eight times stronger, dark on white: what appeared under the overlay.
    public static void SaveDiff(byte[] max, byte[] l, int w, int[] box, string path) {
        int bw = box[2] - box[0] + 1, bh = box[3] - box[1] + 1;
        using (var b = new Bitmap(bw, bh, PixelFormat.Format24bppRgb)) {
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int i = (box[1] + y) * w + box[0] + x;
                    int v = 255 - Math.Min(255, (max[i] - l[i]) * 8);
                    if (v < 0) v = 0;
                    b.SetPixel(x, y, Color.FromArgb(v, v, v));
                }
            b.Save(path, ImageFormat.Png);
        }
    }
}
"@
$inv = [Globalization.CultureInfo]::InvariantCulture
$files = @(Get-ChildItem -LiteralPath $Dir -Filter '*-overlay-*.bmp' | Sort-Object Name)
"overlay bitmaps $($files.Count), bytes $(($files | Measure-Object Length -Sum).Sum)"
# The reference is the brightest value per pixel over the bitmaps of one case and one size (the overlay may change
# its size between cases).
$lumas = @{}
$sizes = @{}
$maxes = @{}
foreach ($f in $files) {
    $w = 0; $h = 0
    $l = [OverlayDiff]::Luma($f.FullName, [ref]$w, [ref]$h)
    $lumas[$f.Name] = $l
    $key = '{0}-{1}x{2}' -f $f.Name.Substring(0, 1), $w, $h
    $sizes[$f.Name] = @($w, $h, $key)
    if (-not $maxes.ContainsKey($key)) { $maxes[$key] = [byte[]]$l.Clone() } else { [OverlayDiff]::Max($maxes[$key], $l) }
}
foreach ($k in ($maxes.Keys | Sort-Object)) { "reference $k from $(@($sizes.Values | Where-Object { $_[2] -eq $k }).Count) bitmaps" }
# The time of each bitmap, from its flash line, in seconds since the window appeared; the overlay's place on screen.
$times = @{}
$origin = @{}
foreach ($c in 'a', 'b', 'c') {
    $log = Get-Content -LiteralPath (Join-Path $Dir "$c-log.txt")
    $start = $log | Where-Object { $_ -match '^start ' } | Select-Object -First 1
    $freq = [double]([regex]::Match($start, 'freq=(\d+)').Groups[1].Value)
    $zero = [long]([regex]::Match(($log | Where-Object { $_ -match '^target window ' } | Select-Object -First 1), 'qpc=(\d+)').Groups[1].Value)
    $ov = $log | Where-Object { $_ -match '^target overlay ' } | Select-Object -First 1
    $origin[$c] = [regex]::Match($ov, 'rect=(-?\d+),(-?\d+)').Groups
    foreach ($l in ($log | Where-Object { $_ -match '^flash target=overlay ' })) {
        $q = [long]([regex]::Match($l, ' qpc=(\d+)').Groups[1].Value)
        $times[[regex]::Match($l, 'bmp=(\S+)').Groups[1].Value] = ($q - $zero) / $freq
    }
}
$rows = foreach ($f in $files) {
    $z = $sizes[$f.Name]
    $d = [OverlayDiff]::Darker($maxes[$z[2]], $lumas[$f.Name], $z[0], $z[1], $Threshold)
    [pscustomobject]@{ name = $f.Name; case = $f.Name.Substring(0, 1); t = $times[$f.Name]; n = $d[0]; box = $d; size = $z }
}
foreach ($c in 'a', 'b', 'c') {
    foreach ($part in @(@{ name = 'start'; lo = -5; hi = 5 }, @{ name = 'end'; lo = 30; hi = 60 })) {
        $sel = @($rows | Where-Object { $_.case -eq $c -and $_.t -ge $part.lo -and $_.t -le $part.hi -and $_.n -gt 0 })
        if (-not $sel.Count) { "case $c $($part.name): no darker pixels"; continue }
        $x0 = ($sel | ForEach-Object { $_.box[1] } | Measure-Object -Minimum).Minimum
        $y0 = ($sel | ForEach-Object { $_.box[2] } | Measure-Object -Minimum).Minimum
        $x1 = ($sel | ForEach-Object { $_.box[3] } | Measure-Object -Maximum).Maximum
        $y1 = ($sel | ForEach-Object { $_.box[4] } | Measure-Object -Maximum).Maximum
        $t0 = ($sel | Measure-Object t -Minimum).Minimum
        $t1 = ($sel | Measure-Object t -Maximum).Maximum
        $ox = [int]$origin[$c][1].Value; $oy = [int]$origin[$c][2].Value
        [string]::Format($inv, 'case {0} {1}: {2} frames from {3:+0.000;-0.000} to {4:+0.000;-0.000} s ({5:F0} ms), at most {6} pixels darker by {7}+, box {8},{9}..{10},{11} in the overlay = screen {12},{13}..{14},{15}',
            $c, $part.name, $sel.Count, $t0, $t1, (1000 * ($t1 - $t0)), ($sel | Measure-Object n -Maximum).Maximum, $Threshold,
            $x0, $y0, $x1, $y1, ($x0 + $ox), ($y0 + $oy), ($x1 + $ox), ($y1 + $oy))
        foreach ($r in ($sel | Sort-Object t)) {
            $b = $r.box
            $area = [Math]::Max(1, ($b[3] - $b[1] + 1) * ($b[4] - $b[2] + 1))
            [string]::Format($inv, '  {0:+0.000;-0.000} s: {1} px, box {2},{3}..{4},{5} (screen {6},{7}..{8},{9}), fill {10:F0} %, mean darker {11}',
                $r.t, $r.n, $b[1], $b[2], $b[3], $b[4], ($b[1] + $ox), ($b[2] + $oy), ($b[3] + $ox), ($b[4] + $oy), (100.0 * $r.n / $area), $b[5])
        }
    }
}
# One picture: case b, the frame nearest -0.45 s (first phase) beside the frame nearest -0.12 s (second phase).
$bRows = @($rows | Where-Object { $_.case -eq 'b' -and $null -ne $_.t })
$p1 = $bRows | Sort-Object { [Math]::Abs($_.t + 0.45) } | Select-Object -First 1
$p2 = $bRows | Sort-Object { [Math]::Abs($_.t + 0.12) } | Select-Object -First 1
if ($p1 -and $p2 -and $p1.size[2] -eq $p2.size[2]) {
    [OverlayDiff]::SavePair($maxes[$p1.size[2]], $lumas[$p1.name], $lumas[$p2.name], $p1.size[0], $p1.size[1], $Out)
    [string]::Format($inv, 'picture {0}: {1} ({2:+0.000;-0.000} s) beside {3} ({4:+0.000;-0.000} s), half size, bytes {5}',
        $Out, $p1.name, $p1.t, $p2.name, $p2.t, (Get-Item -LiteralPath $Out).Length)
}
