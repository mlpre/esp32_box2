Add-Type -AssemblyName System.Drawing
$repo=Split-Path $PSScriptRoot -Parent
$sheet=[System.Drawing.Bitmap]::new(960,320)
$graphics=[System.Drawing.Graphics]::FromImage($sheet)
$names=@('idle','bump','flight','basket')
for($index=0;$index -lt $names.Count;$index++) {
    $bytes=[IO.File]::ReadAllBytes("$repo/build/game_$($names[$index]).ppm")
    $offset=0;$lines=0
    while($lines -lt 3){if($bytes[$offset++] -eq 10){$lines++}}
    $bitmap=[System.Drawing.Bitmap]::new(240,320)
    for($y=0;$y -lt 320;$y++){for($x=0;$x -lt 240;$x++){
        $bitmap.SetPixel($x,$y,[System.Drawing.Color]::FromArgb($bytes[$offset],$bytes[$offset+1],$bytes[$offset+2]));$offset+=3
    }}
    $bitmap.Save("$repo/build/game_$($names[$index]).png")
    $graphics.DrawImageUnscaled($bitmap,$index*240,0)
    $bitmap.Dispose()
}
$sheet.Save("$repo/build/game_preview.png")
$graphics.Dispose();$sheet.Dispose()
