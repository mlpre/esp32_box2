[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments
    exit
}

$root = $PSScriptRoot
$package = Join-Path $root 'out\Package'
$hostSource = Join-Path $root 'out\Host\Box2DisplayHost.exe'
if (!(Test-Path (Join-Path $package 'Box2Display.inf')) -or !(Test-Path $hostSource)) {
    throw 'Build output not found. Run build.ps1 first.'
}

Import-Certificate -FilePath (Join-Path $package 'Box2Display.cer') `
    -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath (Join-Path $package 'Box2Display.cer') `
    -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

$driverArguments = "/add-driver `"$(Join-Path $package 'Box2Display.inf')`" /install"
Start-Process pnputil.exe -ArgumentList $driverArguments -Wait -NoNewWindow

$installDirectory = Join-Path $env:ProgramFiles 'BOX-2 Display'
New-Item -ItemType Directory -Force $installDirectory | Out-Null
$hostTarget = Join-Path $installDirectory 'Box2DisplayHost.exe'
if (Test-Path $hostTarget) {
    & $hostTarget --stop
    Start-Sleep -Milliseconds 500
    $oldHosts = Get-Process Box2DisplayHost -ErrorAction SilentlyContinue
    if ($oldHosts) {
        $oldHosts | Stop-Process -Force
        $oldHosts | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
    }
}
for ($attempt = 0; ; $attempt++) {
    try {
        Copy-Item $hostSource $hostTarget -Force
        break
    } catch {
        if ($attempt -ge 9) { throw }
        Start-Sleep -Milliseconds 300
    }
}

Get-NetFirewallRule -DisplayName 'BOX-2 Wi-Fi Display' -ErrorAction SilentlyContinue |
    Remove-NetFirewallRule
New-NetFirewallRule -DisplayName 'BOX-2 Wi-Fi Display' -Direction Inbound `
    -Program $hostTarget -Action Allow -Profile Any | Out-Null

$taskAction = New-ScheduledTaskAction -Execute $hostTarget
$taskTrigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$taskPrincipal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" `
    -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName Box2DisplayHost -Action $taskAction `
    -Trigger $taskTrigger -Principal $taskPrincipal -Force | Out-Null
Start-ScheduledTask -TaskName Box2DisplayHost

Write-Host 'BOX-2 display driver installed and host started.'
Write-Host 'Open Settings > System > Display and choose Extend these displays.'
