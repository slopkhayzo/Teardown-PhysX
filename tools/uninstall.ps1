# Removes what tools\install.ps1 put into the game folder (only files whose hash still matches).
#   uninstall.ps1 [-GameDir <dir>] [-KeepIni]
param(
    [string]$GameDir = 'D:\Program Files\Games\Steam\steamapps\common\Teardown',
    [switch]$KeepIni
)
$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $GameDir 'TeardownPhysX.install.json'
if (-not (Test-Path $manifestPath)) { Write-Output 'nothing installed'; return }
$m = Get-Content $manifestPath -Raw | ConvertFrom-Json
$left = @()
foreach ($f in $m.files) {
    $p = Join-Path $GameDir $f.name
    if ($KeepIni -and $f.name -eq 'TeardownPhysX.ini') { $left += $f; continue }
    if (-not (Test-Path $p)) { continue }
    if ((Get-FileHash $p -Algorithm SHA256).Hash -ne $f.sha256) {
        Write-Output "kept $p (changed since install)"
        continue
    }
    Remove-Item $p -Force
    Write-Output "removed $p"
}
if ($left.Count -gt 0) {
    [pscustomobject]@{ installed = $m.installed; files = $left } | ConvertTo-Json | Set-Content -Encoding UTF8 $manifestPath
} else {
    Remove-Item $manifestPath -Force
}
