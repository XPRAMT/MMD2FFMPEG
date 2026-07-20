$ErrorActionPreference = 'Stop'

$driversKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Drivers32'
if (Get-ItemProperty -Path $driversKey -Name 'vidc.M2FF' -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $driversKey -Name 'vidc.M2FF'
}
Write-Host 'Removed the per-user MMD2FFMPEG VFW registration.'
$classIdBraced = '{C42D995C-3D1B-4E44-A96B-767B6C2A4646}'
$classIdBare = 'C42D995C-3D1B-4E44-A96B-767B6C2A4646'
$classKey = "HKCU:\Software\Classes\CLSID\$classIdBraced"
$settingsClassKey = 'HKCU:\Software\Classes\CLSID\{65A23874-AE1C-4B10-9F1A-5BC0A8D44B38}'
$mediaObjectKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\$classIdBare"
$oldMediaObjectKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\$classIdBraced"
$categoryKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\Categories\33d9a760-90c8-11d0-bd43-00a0c911ce86\$classIdBare"
$oldCategoryKey = "HKCU:\Software\Classes\DirectShow\MediaObjects\Categories\33d9a760-90c8-11d0-bd43-00a0c911ce86\$classIdBraced"
foreach ($key in @($categoryKey, $oldCategoryKey, $mediaObjectKey, $oldMediaObjectKey, $settingsClassKey, $classKey)) {
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
}
Write-Host 'Removed the per-user MMD2FFMPEG DMO registration.'
