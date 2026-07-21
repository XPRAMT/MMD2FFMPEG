$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
$releaseRoot = Join-Path $projectRoot 'release'
$packageDir = Join-Path $releaseRoot 'MMD2FFMPEG-x64'
$archivePath = Join-Path $releaseRoot 'MMD2FFMPEG-x64.zip'
$requiredFiles = @(
    (Join-Path $buildDir 'mmd2ffmpeg_dmo.dll'),
    (Join-Path $buildDir 'mmd2ffmpeg_cleanup.exe'),
    (Join-Path $buildDir 'MMDLocaleLauncher.exe')
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Build output does not exist: $requiredFile`nRun scripts\\build.ps1 first."
    }
}

New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
foreach ($obsoleteFile in @('install.cmd', 'uninstall.cmd')) {
    $obsoletePath = Join-Path $packageDir $obsoleteFile
    if (Test-Path -LiteralPath $obsoletePath) { Remove-Item -LiteralPath $obsoletePath -Force }
}
$imagesDestination = Join-Path $packageDir 'imgs'
if (Test-Path -LiteralPath $imagesDestination) {
    Remove-Item -LiteralPath $imagesDestination -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $buildDir 'mmd2ffmpeg_dmo.dll') -Destination (Join-Path $packageDir 'mmd2ffmpeg_dmo.dll') -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'mmd2ffmpeg_cleanup.exe') -Destination (Join-Path $packageDir 'mmd2ffmpeg_cleanup.exe') -Force
Copy-Item -LiteralPath (Join-Path $buildDir 'MMDLocaleLauncher.exe') -Destination (Join-Path $packageDir 'MMDLocaleLauncher.exe') -Force
foreach ($fileName in @('install-user.ps1', 'install-user.bat', 'uninstall-user.ps1', 'uninstall-user.bat')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $fileName) -Destination (Join-Path $packageDir $fileName) -Force
}
foreach ($fileName in @('README.md', 'README_TW.md')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $fileName) -Destination (Join-Path $packageDir $fileName) -Force
}

Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $archivePath -Force
Write-Host "Release folder: $packageDir"
Write-Host "Release archive: $archivePath"
