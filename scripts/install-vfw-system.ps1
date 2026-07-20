#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceDll = Join-Path $projectRoot 'build\mmd2ffmpeg.dll'
if (-not (Test-Path -LiteralPath $sourceDll)) {
    throw "Build output does not exist: $sourceDll"
}

$installDir = Join-Path $env:ProgramFiles 'MMD2FFMPEG'
$installedDll = Join-Path $installDir 'mmd2ffmpeg.dll'
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
Copy-Item -LiteralPath $sourceDll -Destination $installedDll -Force

$driversKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32'
New-ItemProperty -Path $driversKey -Name 'VIDC.m2ff' -Value $installedDll -PropertyType String -Force | Out-Null

Write-Host "Installed system-wide x64 VFW codec: $installedDll"
Write-Host 'Restart MMD to refresh its encoder list; no Windows reboot is required.'
