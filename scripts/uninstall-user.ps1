$ErrorActionPreference = 'Stop'

$driversKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Drivers32'
if (Get-ItemProperty -Path $driversKey -Name 'vidc.M2FF' -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $driversKey -Name 'vidc.M2FF'
}
Write-Host 'Removed the per-user MMD2FFMPEG VFW registration.'

