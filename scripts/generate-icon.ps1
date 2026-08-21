param(
    [string]$OutputPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\app-icon.ico')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedRectanglePath([System.Drawing.RectangleF]$Rectangle, [float]$Radius) {
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2
    $path.AddArc($Rectangle.X, $Rectangle.Y, $diameter, $diameter, 180, 90)
    $path.AddArc($Rectangle.Right - $diameter, $Rectangle.Y, $diameter, $diameter, 270, 90)
    $path.AddArc($Rectangle.Right - $diameter, $Rectangle.Bottom - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($Rectangle.X, $Rectangle.Bottom - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

$images = @()
foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $margin = [float]($size * 0.045)
    $rectangle = [System.Drawing.RectangleF]::new($margin, $margin, $size - 2 * $margin, $size - 2 * $margin)
    $path = New-RoundedRectanglePath $rectangle ([float]($size * 0.22))
    $gradient = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        $rectangle,
        [System.Drawing.Color]::FromArgb(37, 99, 235),
        [System.Drawing.Color]::FromArgb(29, 78, 216),
        45.0)
    $graphics.FillPath($gradient, $path)

    $font = [System.Drawing.Font]::new('Segoe UI', [float]($size * 0.56), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $format = [System.Drawing.StringFormat]::new()
    $format.Alignment = [System.Drawing.StringAlignment]::Center
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center
    $textRectangle = [System.Drawing.RectangleF]::new(0, [float](-$size * 0.035), $size, $size)
    $graphics.DrawString('D', $font, [System.Drawing.Brushes]::White, $textRectangle, $format)
    $dotSize = [float]([Math]::Max(2, $size * 0.10))
    $graphics.FillEllipse([System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(103, 232, 249)),
                          [float]($size * 0.72), [float]($size * 0.20), $dotSize, $dotSize)

    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += ,$stream.ToArray()
    $format.Dispose()
    $font.Dispose()
    $gradient.Dispose()
    $path.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

$parent = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$file = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create)
$writer = [System.IO.BinaryWriter]::new($file)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
for ($index = 0; $index -lt $images.Count; $index++) {
    $size = @(16, 24, 32, 48, 64, 128, 256)[$index]
    $writer.Write([byte]($(if ($size -eq 256) { 0 } else { $size })))
    $writer.Write([byte]($(if ($size -eq 256) { 0 } else { $size })))
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$index].Length)
    $writer.Write([uint32]$offset)
    $offset += $images[$index].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Dispose()
$file.Dispose()

Write-Output "Generated: $OutputPath"
