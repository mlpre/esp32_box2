[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments
    exit
}

$installDirectory = Join-Path $env:ProgramFiles 'BOX-2 Display'
$host = Join-Path $installDirectory 'Box2DisplayHost.exe'
if (Test-Path $host) {
    & $host --stop
    Start-Sleep -Milliseconds 500
}

Unregister-ScheduledTask -TaskName Box2DisplayHost -Confirm:$false `
    -ErrorAction SilentlyContinue
Get-NetFirewallRule -DisplayName 'BOX-2 Wi-Fi Display' -ErrorAction SilentlyContinue |
    Remove-NetFirewallRule

$driver = Get-WindowsDriver -Online |
    Where-Object OriginalFileName -like '*Box2Display.inf' |
    Select-Object -First 1
if ($driver) {
    & pnputil.exe /delete-driver $driver.Driver /uninstall /force
}

if (Test-Path $installDirectory) {
    Remove-Item -LiteralPath $installDirectory -Recurse -Force
}
Write-Host 'BOX-2 display driver removed.'
