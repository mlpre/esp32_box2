# Pack the original transparent PNG pixels without changing the source artwork.
Add-Type -AssemblyName System.Drawing
$repo = Split-Path $PSScriptRoot -Parent
$out = [System.Text.StringBuilder]::new()
[void]$out.AppendLine('#pragma once')
[void]$out.AppendLine('#include <stdint.h>')
foreach ($side in @('left','right')) {
    $bitmap=[System.Drawing.Bitmap]::new("$repo/main/assets/back_$side.png")
    [void]$out.AppendLine("static const uint32_t back_$($side)[$($bitmap.Width*$bitmap.Height)] = {")
    for($y=0;$y -lt $bitmap.Height;$y++) {
        $row=for($x=0;$x -lt $bitmap.Width;$x++) {
            $p=$bitmap.GetPixel($x,$y)
            $argb=[uint32]([long]$p.A*16777216+[long]$p.R*65536+[long]$p.G*256+$p.B)
            '0x{0:x8}u' -f $argb
        }
        [void]$out.AppendLine(($row -join ',')+',')
    }
    [void]$out.AppendLine('};')
    $bitmap.Dispose()
}
[System.IO.File]::WriteAllText("$repo/main/game_sprites.h",$out.ToString())
Write-Output 'Packed two 80x120 rear-view sprites'
