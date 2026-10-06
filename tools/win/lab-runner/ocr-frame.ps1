# Full-resolution OCR of the lab's screen for the interactive game channel (owner, 2026-10-01: the Jev pilot must
# work from general perception, not scenario rules). Windows.Media.Ocr in process, no window. The frame is taken
# at scale 1 from the lab overlay's screenshot service (overlay hidden), read, and deleted at once: only text and
# boxes leave the lab, never the image (owner: screenshots resized before transmission).
# Dot-sourced by game-runtime.ps1 (action "ocr"); run directly it reads the current screen once and prints the JSON.
# Output: {"w":W,"h":H,"ms":MS,"lines":[{"t":"text","b":[x0,y0,x1,y1]}...]}, boxes as fractions of the frame,
# at most 80 lines of at most 120 characters, in the engine's reading order.
param([string]$Png = '')

$script:ocrState = @{ ready = $false; engine = $null; asTask = $null; lang = '' }

function Ocr-Await($op, [Type]$type) {
    $m = $script:ocrState.asTask.MakeGenericMethod($type)
    $task = $m.Invoke($null, @($op))
    if (!$task.Wait(15000)) { throw 'OCR call timed out after 15 s' }
    return $task.Result
}

function Ocr-Init {
    if ($script:ocrState.ready) { return }
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
    $null = [Windows.Storage.FileAccessMode, Windows.Storage, ContentType = WindowsRuntime]
    $null = [Windows.Storage.Streams.IRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
    $null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
    $null = [Windows.Media.Ocr.OcrResult, Windows.Foundation, ContentType = WindowsRuntime]
    $null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics, ContentType = WindowsRuntime]
    $null = [Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics, ContentType = WindowsRuntime]
    $script:ocrState.asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
    # The lab's games run in English (en-US OCR is installed; the user profile's first language, pl, misreads
    # English words): en-US unless $env:BC250_OCR_LANG names another installed recognizer.
    $null = [Windows.Globalization.Language, Windows.Globalization, ContentType = WindowsRuntime]
    $tag = if ($env:BC250_OCR_LANG) { $env:BC250_OCR_LANG } else { 'en-US' }
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage((New-Object Windows.Globalization.Language $tag))
    if (!$engine) { $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages() }
    if (!$engine) { throw 'no OCR engine for en-US or the user profile languages' }
    $script:ocrState.engine = $engine
    $script:ocrState.lang = $engine.RecognizerLanguage.LanguageTag
    $script:ocrState.ready = $true
}

# Reads one PNG file; returns @{json; count; ms; lang}.
function Get-OcrLines([string]$Path) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Ocr-Init
    $file = Ocr-Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Path)) ([Windows.Storage.StorageFile])
    $stream = Ocr-Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    try {
        $dec = Ocr-Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $bmp = Ocr-Await ($dec.GetSoftwareBitmapAsync([Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8,
                [Windows.Graphics.Imaging.BitmapAlphaMode]::Premultiplied)) ([Windows.Graphics.Imaging.SoftwareBitmap])
        $w = [double]$dec.PixelWidth; $h = [double]$dec.PixelHeight
        $res = Ocr-Await ($script:ocrState.engine.RecognizeAsync($bmp)) ([Windows.Media.Ocr.OcrResult])
    } finally { $stream.Dispose() }
    $lines = @()
    foreach ($l in $res.Lines) {
        $x0 = 1e9; $y0 = 1e9; $x1 = 0; $y1 = 0
        foreach ($wd in $l.Words) {
            $r = $wd.BoundingRect
            $x0 = [math]::Min($x0, $r.X); $y0 = [math]::Min($y0, $r.Y)
            $x1 = [math]::Max($x1, $r.X + $r.Width); $y1 = [math]::Max($y1, $r.Y + $r.Height)
        }
        $t = [string]$l.Text; if ($t.Length -gt 120) { $t = $t.Substring(0, 120) }
        $lines += , ([ordered]@{ t = $t; b = @([math]::Round($x0 / $w, 3), [math]::Round($y0 / $h, 3),
                    [math]::Round($x1 / $w, 3), [math]::Round($y1 / $h, 3)) })
        if ($lines.Count -ge 80) { break }
    }
    $ms = [int]$sw.ElapsedMilliseconds
    $json = ConvertTo-Json -InputObject ([ordered]@{ w = $w; h = $h; ms = $ms; lang = $script:ocrState.lang; lines = $lines }) -Depth 5 -Compress
    return @{ json = $json; count = $lines.Count; ms = $ms; lang = $script:ocrState.lang }
}

# Takes the frame at scale 1 (overlay hidden) into $Path, reads it, deletes it.
function Get-ScreenOcr([string]$Path) {
    Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile $Path -TimeoutSec 8
    try { return Get-OcrLines $Path } finally { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue }
}

if ($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference = 'Stop'
    $o = if ($Png) { Get-OcrLines $Png } else { Get-ScreenOcr (Join-Path $env:TEMP ('ocr-test-{0}.png' -f $PID)) }
    'ocr {0} lines {1} ms lang {2}' -f $o.count, $o.ms, $o.lang
    $o.json
}
