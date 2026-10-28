# Builds the release package from the last tools\build.cmd (build\: the plugin and PhysX's GPU module) and
# tools\package\ (the settings file, the player README, the loader's licence):
#   dist\TeardownPhysX-<version>\          the files a player copies into the game folder, README.txt, licenses\
#   dist\TeardownPhysX-<version>.zip       the same, to publish
#   dist\TeardownPhysX-<version>-symbols\  the plugin's .pdb and .map (not shipped: for players' crash dumps)
#   package.ps1 [-AllowMissingArchs]
# It refuses a GPU module without machine code for every GPU generation PhysX supports (tools\build_physx_gpu.cmd's
# default) or a plugin whose version resource is not src\core\version.h's. No game launch: it needs no queue.
param([switch]$AllowMissingArchs)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path (Join-Path $repo 'src\core\version.h') -Pattern '#define TDPX_VERSION "([^"]+)"').Matches[0].Groups[1].Value
$name = "TeardownPhysX-$version"
$dist = Join-Path $repo 'dist'
$out = Join-Path $dist $name
$asi = Join-Path $repo 'build\TeardownPhysX.asi'
$gpu = Join-Path $repo 'build\PhysXGpu_64.dll'
$loader = Join-Path $repo 'third_party\uasi\v9.7.4\dinput8.dll'  # Ultimate ASI Loader x64, shipped as dxgi.dll
foreach ($f in $asi, $gpu, $loader) { if (-not (Test-Path $f)) { throw "missing $f" } }

$fileVersion = (Get-Item $asi).VersionInfo.ProductVersion
if ($fileVersion -ne $version) { throw "build\TeardownPhysX.asi is version '$fileVersion', src\core\version.h says ${version}: rebuild" }

# GPU machine code: Volta (sm_70) to Blackwell (sm_120), and PTX for newer GPUs
$cuobjdump = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\cuobjdump.exe'
$wantSass = 'sm_70', 'sm_75', 'sm_86', 'sm_89', 'sm_120'
$wantPtx = 'sm_120'
if (Test-Path $cuobjdump) {
    $sass = @(& $cuobjdump -lelf $gpu | ForEach-Object { if ($_ -match '\.(sm_\d+)\.cubin') { $Matches[1] } } | Sort-Object -Unique)
    $ptx = @(& $cuobjdump -lptx $gpu | ForEach-Object { if ($_ -match '\.(sm_\d+)\.ptx') { $Matches[1] } } | Sort-Object -Unique)
    Write-Output "GPU module: machine code $($sass -join ', '); PTX $($ptx -join ', ')"
    $missing = @($wantSass | Where-Object { $sass -notcontains $_ }) + @($wantPtx | Where-Object { $ptx -notcontains $_ } | ForEach-Object { "PTX $_" })
    if ($missing -and -not $AllowMissingArchs) {
        throw "the GPU module lacks $($missing -join ', '): build PhysX with tools\build_physx_gpu.cmd's default architectures"
    }
} elseif (-not $AllowMissingArchs) {
    throw "cuobjdump not found ($cuobjdump): can't check the GPU module's architectures (-AllowMissingArchs skips it)"
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force (Join-Path $out 'licenses') | Out-Null
Copy-Item $loader (Join-Path $out 'dxgi.dll')
Copy-Item $asi $out
Copy-Item $gpu $out
# text files with Windows line endings, for Notepad
function Copy-Text($from, $to, $replace = @{}) {
    $t = [IO.File]::ReadAllText($from)
    foreach ($k in $replace.Keys) { $t = $t.Replace($k, $replace[$k]) }
    [IO.File]::WriteAllText($to, ($t -replace "`r?`n", "`r`n"), [Text.UTF8Encoding]::new($false))
}
Copy-Text (Join-Path $PSScriptRoot 'package\TeardownPhysX.ini') (Join-Path $out 'TeardownPhysX.ini')
Copy-Text (Join-Path $PSScriptRoot 'package\README.txt') (Join-Path $out 'README.txt') @{ '@VERSION@' = $version }
Copy-Text (Join-Path $repo 'LICENSE') (Join-Path $out 'licenses\Teardown-PhysX.txt')
Copy-Text (Join-Path $repo 'third_party\physx-sdk\physx\LICENSE.md') (Join-Path $out 'licenses\PhysX.txt')
Copy-Text (Join-Path $repo 'third_party\minhook\LICENSE.txt') (Join-Path $out 'licenses\MinHook.txt')
Copy-Text (Join-Path $PSScriptRoot 'package\licenses\Ultimate-ASI-Loader.txt') (Join-Path $out 'licenses\Ultimate-ASI-Loader.txt')

$symbols = Join-Path $dist "$name-symbols"
New-Item -ItemType Directory -Force $symbols | Out-Null
foreach ($ext in 'pdb', 'map') {
    $side = [IO.Path]::ChangeExtension($asi, $ext)
    if (Test-Path $side) { Copy-Item $side $symbols -Force }
}

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal
Get-ChildItem $out -Recurse -File | ForEach-Object {
    '{0,12:N0}  {1}' -f $_.Length, $_.FullName.Substring($out.Length + 1)
}
'{0,12:N0}  {1} (SHA-256 {2})' -f (Get-Item $zip).Length, $zip, (Get-FileHash $zip -Algorithm SHA256).Hash
