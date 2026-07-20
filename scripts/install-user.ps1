$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $projectRoot 'build\mmd2ffmpeg.dll'
$dmoDll = Join-Path $projectRoot 'build\mmd2ffmpeg_dmo.dll'
$cleanupExe = Join-Path $projectRoot 'build\mmd2ffmpeg_cleanup.exe'
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Build output does not exist: $dll"
}
if (-not (Test-Path -LiteralPath $dmoDll)) {
    throw "DMO build output does not exist: $dmoDll"
}
if (-not (Test-Path -LiteralPath $cleanupExe)) {
    throw "Cleanup helper does not exist: $cleanupExe"
}
$installDir = Join-Path $env:LOCALAPPDATA 'MMD2FFMPEG'
$installedDll = Join-Path $installDir 'mmd2ffmpeg.dll'
$installedDmoDll = Join-Path $installDir 'mmd2ffmpeg_dmo.dll'
$installedCleanupExe = Join-Path $installDir 'mmd2ffmpeg_cleanup.exe'
$config = Join-Path $installDir 'config.ini'
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination $installedDll -Force
Copy-Item -LiteralPath $dmoDll -Destination $installedDmoDll -Force
Copy-Item -LiteralPath $cleanupExe -Destination $installedCleanupExe -Force
if (-not (Test-Path -LiteralPath $config)) {
    $defaultConfig = @'
ffmpeg=ffmpeg.exe
output=C:\APP\MMD\MMD2FFMPEG\out\mmd-output.mkv
fps=30
backend=nvenc
codec=hevc
bit_depth=10
preset=7
rate_control=qp
qp=20
bitrate_kbps=20000
video_args=-c:v hevc_nvenc -profile:v main10 -preset p7 -tune hq -rc constqp -qp 20
'@
    [System.IO.File]::WriteAllText($config, $defaultConfig, [System.Text.UTF8Encoding]::new($true))
}
$configText = [System.IO.File]::ReadAllText($config)
$legacyFfmpegLine = 'ffmpeg=C:\Program Files\Hybrid\64bit\ffmpeg.exe'
if ($configText -match ('(?m)^' + [regex]::Escape($legacyFfmpegLine) + '\r?$')) {
    $configText = [regex]::Replace($configText, '(?m)^' + [regex]::Escape($legacyFfmpegLine) + '\r?$', 'ffmpeg=ffmpeg.exe')
    [System.IO.File]::WriteAllText($config, $configText, [System.Text.UTF8Encoding]::new($true))
}
$configText = [regex]::Replace($configText, '(?m)^follow_avi_path=.*\r?\n?', '')
[System.IO.File]::WriteAllText($config, $configText, [System.Text.UTF8Encoding]::new($true))
$driversKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Drivers32'
New-Item -Path $driversKey -Force | Out-Null
New-ItemProperty -Path $driversKey -Name 'vidc.m2ff' -Value $installedDll -PropertyType String -Force | Out-Null
$classIdBraced = '{C42D995C-3D1B-4E44-A96B-767B6C2A4646}'
$classIdBare = 'C42D995C-3D1B-4E44-A96B-767B6C2A4646'
$classKey = "HKCU:\Software\Classes\CLSID\$classIdBraced"
$serverKey = Join-Path $classKey 'InprocServer32'
New-Item -Path $serverKey -Force | Out-Null
Set-Item -Path $classKey -Value 'MMD2FFMPEG DMO Encoder'
Set-Item -Path $serverKey -Value $installedDmoDll
New-ItemProperty -Path $serverKey -Name 'ThreadingModel' -Value 'Both' -PropertyType String -Force | Out-Null
$settingsClassId = '{65A23874-AE1C-4B10-9F1A-5BC0A8D44B38}'
$settingsClassKey = "HKCU:\Software\Classes\CLSID\$settingsClassId"
$settingsServerKey = Join-Path $settingsClassKey 'InprocServer32'
New-Item -Path $settingsServerKey -Force | Out-Null
Set-Item -Path $settingsClassKey -Value 'MMD2FFMPEG Encoder Settings'
Set-Item -Path $settingsServerKey -Value $installedDmoDll
New-ItemProperty -Path $settingsServerKey -Name 'ThreadingModel' -Value 'Both' -PropertyType String -Force | Out-Null

$oldMediaObjectKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\$classIdBraced"
if (Test-Path -LiteralPath $oldMediaObjectKey) { Remove-Item -LiteralPath $oldMediaObjectKey -Recurse -Force }
$mediaObjectKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\$classIdBare"
New-Item -Path $mediaObjectKey -Force | Out-Null
Set-Item -Path $mediaObjectKey -Value 'MMD2FFMPEG DMO Encoder'
$videoMajor = [Guid]'73646976-0000-0010-8000-00AA00389B71'
$rgb24 = [Guid]'e436eb7d-524f-11ce-9f53-0020af0ba770'
$rgb32 = [Guid]'e436eb7e-524f-11ce-9f53-0020af0ba770'
[byte[]]$inputTypes = $videoMajor.ToByteArray() + $rgb32.ToByteArray() + $videoMajor.ToByteArray() + $rgb24.ToByteArray()
$m2ff = [Guid]'4646324d-0000-0010-8000-00aa00389b71'
[byte[]]$outputTypes = $videoMajor.ToByteArray() + $m2ff.ToByteArray()
New-ItemProperty -Path $mediaObjectKey -Name 'InputTypes' -Value $inputTypes -PropertyType Binary -Force | Out-Null
New-ItemProperty -Path $mediaObjectKey -Name 'OutputTypes' -Value $outputTypes -PropertyType Binary -Force | Out-Null
$oldCategoryKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\Categories\33d9a760-90c8-11d0-bd43-00a0c911ce86\$classIdBraced"
if (Test-Path -LiteralPath $oldCategoryKey) { Remove-Item -LiteralPath $oldCategoryKey -Recurse -Force }
$categoryKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\Categories\33d9a760-90c8-11d0-bd43-00a0c911ce86\$classIdBare"
New-Item -Path $categoryKey -Force | Out-Null
Write-Host "Installed x64 VFW codec: $installedDll"
Write-Host "Installed x64 DMO encoder: $installedDmoDll"
Write-Host "Configuration: $config"
Write-Host 'Restart MMD if it was already open; no Windows reboot is required.'
