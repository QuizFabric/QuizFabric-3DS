# Regenerates the CIA/SMDH artwork and the silent banner audio.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File assets\make_assets.ps1
Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function New-GradientImage {
    param(
        [int]$Width,
        [int]$Height,
        [string]$Title,
        [string]$Subtitle,
        [single]$TitleSize,
        [single]$SubtitleSize,
        [string]$OutPath
    )

    $bmp = New-Object System.Drawing.Bitmap($Width, $Height)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias

    # Pink -> purple diagonal, matching the in-app background.
    $rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
    $c1 = [System.Drawing.Color]::FromArgb(255, 255, 122, 198)
    $c2 = [System.Drawing.Color]::FromArgb(255, 96, 42, 176)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45.0)
    $g.FillRectangle($brush, $rect)

    # A few translucent bubbles for depth.
    $bubble = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(38, 255, 255, 255))
    $rand = New-Object System.Random(1234)
    for ($i = 0; $i -lt 9; $i++) {
        $r = $rand.Next([int]($Height / 9), [int]($Height / 3))
        $x = $rand.Next(-$r, $Width)
        $y = $rand.Next(-$r, $Height)
        $g.FillEllipse($bubble, $x, $y, $r, $r)
    }

    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment     = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center

    $white  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $shadow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(120, 40, 8, 60))

    $family = "Segoe UI"
    try { $test = New-Object System.Drawing.FontFamily($family) } catch { $family = "Arial" }

    if ($Subtitle) {
        $titleFont = New-Object System.Drawing.Font($family, $TitleSize, [System.Drawing.FontStyle]::Bold)
        $subFont   = New-Object System.Drawing.Font($family, $SubtitleSize, [System.Drawing.FontStyle]::Regular)
        $tRect = New-Object System.Drawing.RectangleF(0, ($Height * 0.30), $Width, ($Height * 0.34))
        $sRect = New-Object System.Drawing.RectangleF(0, ($Height * 0.62), $Width, ($Height * 0.22))
        $tShadow = New-Object System.Drawing.RectangleF(2, ($Height * 0.30 + 2), $Width, ($Height * 0.34))
        $g.DrawString($Title, $titleFont, $shadow, $tShadow, $fmt)
        $g.DrawString($Title, $titleFont, $white, $tRect, $fmt)
        $g.DrawString($Subtitle, $subFont, $white, $sRect, $fmt)
    } else {
        $titleFont = New-Object System.Drawing.Font($family, $TitleSize, [System.Drawing.FontStyle]::Bold)
        $tRect = New-Object System.Drawing.RectangleF(0, 0, $Width, $Height)
        $tShadow = New-Object System.Drawing.RectangleF(1, 2, $Width, $Height)
        $g.DrawString($Title, $titleFont, $shadow, $tShadow, $fmt)
        $g.DrawString($Title, $titleFont, $white, $tRect, $fmt)
    }

    $g.Dispose()
    $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("  wrote {0} ({1}x{2})" -f (Split-Path -Leaf $OutPath), $Width, $Height)
}

# SMDH icon: 48x48 is the size the HOME menu and Homebrew Launcher use.
New-GradientImage -Width 48 -Height 48 -Title "Q" -Subtitle "" `
    -TitleSize 26 -SubtitleSize 0 -OutPath (Join-Path $here "icon.png")

# CIA banner: 256x128.
New-GradientImage -Width 256 -Height 128 -Title "QuizFabric" -Subtitle "3DS CLIENT" `
    -TitleSize 26 -SubtitleSize 10 -OutPath (Join-Path $here "banner.png")

# Banner audio. bannertool wants a PCM WAV; one second of silence keeps the
# HOME menu quiet instead of looping a jingle.
$sampleRate = 32000
$channels   = 2
$bits       = 16
$seconds    = 1
$dataSize   = $sampleRate * $channels * ($bits / 8) * $seconds
$wavPath    = Join-Path $here "audio.wav"

$fs = [System.IO.File]::Create($wavPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([char[]]"RIFF")
$bw.Write([int](36 + $dataSize))
$bw.Write([char[]]"WAVE")
$bw.Write([char[]]"fmt ")
$bw.Write([int]16)                                            # PCM chunk size
$bw.Write([int16]1)                                           # format = PCM
$bw.Write([int16]$channels)
$bw.Write([int]$sampleRate)
$bw.Write([int]($sampleRate * $channels * ($bits / 8)))       # byte rate
$bw.Write([int16]($channels * ($bits / 8)))                   # block align
$bw.Write([int16]$bits)
$bw.Write([char[]]"data")
$bw.Write([int]$dataSize)
$bw.Write((New-Object byte[] $dataSize))
$bw.Close()
$fs.Close()
Write-Host ("  wrote audio.wav ({0} bytes, {1}s silence)" -f $dataSize, $seconds)

Write-Host ""
Write-Host "Assets regenerated."
