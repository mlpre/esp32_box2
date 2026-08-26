[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) {
    throw 'Visual Studio Build Tools 2022 is not installed.'
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (!$msbuild) {
    throw 'MSBuild was not found.'
}

& $msbuild (Join-Path $root 'Host\Box2DisplayHost.vcxproj') /m /t:Rebuild /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Host build failed.' }

& $msbuild (Join-Path $root 'Driver\Box2Display.vcxproj') /m /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off
if ($LASTEXITCODE -ne 0) { throw 'Driver build failed.' }

$package = Join-Path $root 'out\Package'
New-Item -ItemType Directory -Force $package | Out-Null
Copy-Item (Join-Path $root 'out\Driver\Box2Display.dll') $package -Force
Copy-Item (Join-Path $root 'out\Driver\Box2Display.inf') $package -Force

$kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$kitVersion = Get-ChildItem $kitsBin -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'x86\Inf2Cat.exe') } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (!$kitVersion) { throw 'Inf2Cat was not found. Install Windows Driver Kit.' }

$inf2cat = Join-Path $kitVersion.FullName 'x86\Inf2Cat.exe'
$signtool = Join-Path $kitVersion.FullName 'x64\signtool.exe'
& $inf2cat /uselocaltime /driver:$package /os:10_X64
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed.' }

$subject = 'CN=BOX-2 Display Development'
$certificate = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object Subject -eq $subject |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1
if (!$certificate) {
    $certificate = New-SelfSignedCertificate -Type CodeSigningCert `
        -Subject $subject -CertStoreLocation Cert:\CurrentUser\My `
        -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}

$certificatePath = Join-Path $package 'Box2Display.cer'
Export-Certificate -Cert $certificate -FilePath $certificatePath -Force | Out-Null
& $signtool sign /fd SHA256 /sha1 $certificate.Thumbprint /s My `
    (Join-Path $package 'Box2Display.cat')
if ($LASTEXITCODE -ne 0) { throw 'Driver catalog signing failed.' }

Write-Host "Build complete: $package"
