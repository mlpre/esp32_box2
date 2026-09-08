param(
    [Parameter(Mandatory=$true)][string]$Background,
    [Parameter(Mandatory=$true)][string]$Voice
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
# Keep one chorus hook (27.50-31.78 s), excluding introductions and long backing sections.
# Tiny edge fades avoid clicks without obscuring the phrase.
& ffmpeg -hide_banner -loglevel error -y -i $Background -vn -ac 1 -ar 24000 -af 'atrim=start=27.50:end=31.78,asetpts=PTS-STARTPTS,volume=8dB,afade=t=in:d=0.012,afade=t=out:st=4.25:d=0.03' -f s16le "$repo/main/assets/bgm.pcm"
if($LASTEXITCODE -ne 0){throw 'Background conversion failed'}
& ffmpeg -hide_banner -loglevel error -y -i $Voice -t 1.20 -vn -ac 1 -ar 24000 -af 'afade=t=in:d=0.005,afade=t=out:st=1.17:d=0.03' -f s16le "$repo/main/assets/voice.pcm"
if($LASTEXITCODE -ne 0){throw 'Voice conversion failed'}
foreach($clip in @(@('bgm',205440),@('voice',57600))) {
    $bytes=[IO.File]::ReadAllBytes("$repo/main/assets/$($clip[0]).pcm")
    if($bytes.Length -ne $clip[1]){throw "Unexpected $($clip[0]) duration"}
    $peak=0
    for($i=0;$i -lt $bytes.Length;$i+=2){$sample=[Math]::Abs([int][BitConverter]::ToInt16($bytes,$i));if($sample -gt $peak){$peak=$sample}}
    if($peak -lt 1000){throw "$($clip[0]) is silent or too quiet"}
    Write-Output "$($clip[0]): $($bytes.Length/48000) seconds, peak=$peak"
}
