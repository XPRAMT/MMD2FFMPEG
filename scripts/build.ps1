$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw 'Visual Studio C++ x64 build tools are not installed.'
}
$devShell = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) {
    throw "Visual Studio CMake was not found: $cmake"
}
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$command = '"{0}" -arch=amd64 -host_arch=amd64 && "{1}" -S "{2}" -B "{3}" -G Ninja && "{1}" --build "{3}" --config Release' -f $devShell, $cmake, $projectRoot, $buildDir
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

