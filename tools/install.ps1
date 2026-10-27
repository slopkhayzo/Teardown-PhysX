# Installs Ultimate ASI Loader and TeardownPhysX.asi (+ an ini) into the game folder.
# Loader name: dxgi.dll (teardown.exe imports it, so it loads at process start, before any game code; winmm.dll
# loads later, during Steam init, and DSOUND is imported by ordinal, which the loader's exports do not match).
# Refuses to overwrite files it did not put there; records what it installed in TeardownPhysX.install.json
# so tools\uninstall.ps1 removes exactly that.
# PhysX's GPU module (PhysXGpu_64.dll, from the GPU-enabled build) goes next to the plugin when there is one; the
# plugin loads it only with [PhysX] Gpu=1.
#   install.ps1 [-Asi <path>] [-Ini <path>] [-GameDir <dir>] [-LoaderName dxgi.dll] [-GpuDll <path>|none]
param(
    [string]$Asi = '',
    [string]$Ini = '',
    [string]$GameDir = 'D:\Program Files\Games\Steam\steamapps\common\Teardown',
    [string]$LoaderName = 'dxgi.dll',
    [string]$GpuDll = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Asi) { $Asi = Join-Path $repo 'build\TeardownPhysX.asi' }
$loader = Join-Path $repo 'third_party\uasi\v9.7.4\dinput8.dll'
$manifestPath = Join-Path $GameDir 'TeardownPhysX.install.json'

$old = @{}
if (Test-Path $manifestPath) {
    (Get-Content $manifestPath -Raw | ConvertFrom-Json).files | ForEach-Object { $old[$_.name] = $_.sha256 }
}

$plan = @(
    @{ name = $LoaderName; src = $loader },
    @{ name = 'TeardownPhysX.asi'; src = $Asi }
)
if ($Ini) { $plan += @{ name = 'TeardownPhysX.ini'; src = $Ini } }
if (-not $GpuDll) { $GpuDll = Join-Path $repo 'build\PhysXGpu_64.dll' }
if ($GpuDll -ne 'none' -and (Test-Path $GpuDll)) { $plan += @{ name = 'PhysXGpu_64.dll'; src = $GpuDll } }

$files = @()
foreach ($p in $plan) {
    if (-not (Test-Path $p.src)) { throw "missing $($p.src)" }
    $dst = Join-Path $GameDir $p.name
    if ((Test-Path $dst) -and -not $old.ContainsKey($p.name)) {
        throw "$dst exists and was not installed by this tool: not touching it"
    }
    if ((Test-Path $dst) -and $old.ContainsKey($p.name)) {
        $cur = (Get-FileHash $dst -Algorithm SHA256).Hash
        if ($cur -ne $old[$p.name]) { throw "$dst changed since it was installed: not touching it" }
    }
    Copy-Item $p.src $dst -Force
    $files += [pscustomobject]@{ name = $p.name; sha256 = (Get-FileHash $dst -Algorithm SHA256).Hash }
}
# keep entries from an earlier install that this run did not replace (e.g. an ini)
foreach ($k in $old.Keys) {
    if (-not ($files | Where-Object name -eq $k)) { $files += [pscustomobject]@{ name = $k; sha256 = $old[$k] } }
}
[pscustomobject]@{ installed = (Get-Date -Format s); files = $files } | ConvertTo-Json | Set-Content -Encoding UTF8 $manifestPath
Write-Output "installed into $GameDir : $(($files | ForEach-Object name) -join ', ')"
