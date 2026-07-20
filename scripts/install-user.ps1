$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $projectRoot 'build\mmd2ffmpeg.dll'
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Build output does not exist: $dll"
}
$installDir = Join-Path $env:LOCALAPPDATA 'MMD2FFMPEG'
$installedDll = Join-Path $installDir 'mmd2ffmpeg.dll'
$config = Join-Path $installDir 'config.ini'
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination $installedDll -Force
if (-not (Test-Path -LiteralPath $config)) {
    $defaultConfig = @'
ffmpeg=C:\Program Files\Hybrid\64bit\ffmpeg.exe
output=C:\APP\MMD\MMD2FFMPEG\out\mmd-output.mp4
fps=30
video_args=-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -movflags +faststart
'@
    [System.IO.File]::WriteAllText($config, $defaultConfig, [System.Text.UTF8Encoding]::new($true))
}
$driversKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Drivers32'
New-Item -Path $driversKey -Force | Out-Null
New-ItemProperty -Path $driversKey -Name 'vidc.M2FF' -Value $installedDll -PropertyType String -Force | Out-Null
Write-Host "Installed x64 VFW codec: $installedDll"
Write-Host "Configuration: $config"
Write-Host 'Restart MMD if it was already open; no Windows reboot is required.'

