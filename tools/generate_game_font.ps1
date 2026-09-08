# Build only glyphs used by the game. No runtime font dependency.
Add-Type -AssemblyName System.Drawing
$repo = Split-Path $PSScriptRoot -Parent
$source = Get-Content -LiteralPath "$repo/main/game_render.c" -Raw -Encoding utf8
$chars = [System.Collections.Generic.SortedSet[int]]::new()
32..126 | ForEach-Object { [void]$chars.Add($_) }
foreach ($c in $source.ToCharArray()) { if ([int]$c -gt 127) { [void]$chars.Add([int]$c) } }
$fontCn = [System.Drawing.Font]::new('Microsoft YaHei',14,[System.Drawing.FontStyle]::Regular,[System.Drawing.GraphicsUnit]::Pixel)
$fontEn = [System.Drawing.Font]::new('Consolas',13,[System.Drawing.FontStyle]::Bold,[System.Drawing.GraphicsUnit]::Pixel)
$bitmap = [System.Drawing.Bitmap]::new(16,16)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
$format = [System.Drawing.StringFormat]::GenericTypographic.Clone()
$format.FormatFlags = [System.Drawing.StringFormatFlags]::NoClip
$out = [System.Text.StringBuilder]::new()
[void]$out.AppendLine('#pragma once')
[void]$out.AppendLine('#include <stdint.h>')
[void]$out.AppendLine('static const struct { uint16_t code; uint16_t rows[16]; } game_font[] = {')
foreach ($cp in $chars) {
    $graphics.Clear([System.Drawing.Color]::Black)
    $font = if ($cp -lt 128) { $fontEn } else { $fontCn }
    $graphics.DrawString([string][char]$cp,$font,[System.Drawing.Brushes]::White,[System.Drawing.PointF]::new(0,-1),$format)
    $rows = for ($y=0;$y -lt 16;$y++) {
        $bits=0
        for ($x=0;$x -lt 16;$x++) { if ($bitmap.GetPixel($x,$y).R -gt 127) { $bits = $bits -bor (1 -shl $x) } }
        '0x{0:x4}' -f $bits
    }
    [void]$out.AppendLine(('{{0x{0:x4}, {{{1}}}}},' -f $cp,($rows -join ',')))
}
[void]$out.AppendLine('};')
[void]$out.AppendLine('#define FONT_COUNT (sizeof(game_font)/sizeof(game_font[0]))')
[System.IO.File]::WriteAllText("$repo/main/game_font.h",$out.ToString())
$graphics.Dispose(); $bitmap.Dispose(); $fontCn.Dispose(); $fontEn.Dispose(); $format.Dispose()
Write-Output "Generated $($chars.Count) glyphs"
