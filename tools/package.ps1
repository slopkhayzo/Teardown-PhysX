# Builds the two release archives from the last tools\build.cmd (build\: the plugin and PhysX's GPU module),
# tools\package\ (the settings file, the player README, the loader's notice) and the loader in third_party\uasi\v9.7.4\:
#   dist\Teardown-PhysX-v<version>-plugin-only.zip                      for a game that already has an ASI loader
#   dist\Teardown-PhysX-v<version>-with-Ultimate-ASI-Loader-v9.7.4.zip  the same and the loader, as dxgi.dll
#   dist\<archive>.zip.sha256                                           the archive's SHA-256 (sha256sum's format)
#   dist\Teardown-PhysX-v<version>-symbols\  the plugin's .pdb and .map (not shipped: for players' crash dumps)
# Each archive holds README.txt, licenses\ and SHA256SUMS.txt (the SHA-256 of its other files); they are staged in
# build\release-staging\.
#   package.ps1 [-AllowMissingArchs]
# It refuses a GPU module without machine code for every GPU generation PhysX supports (tools\build_physx_gpu.cmd's
# default), a plugin whose version resource is not src\core\version.h's, a loader other than the tested one and an
# archive that exists already (a published archive keeps its hash: delete it to rebuild). No game launch: it needs no
# queue.
param([switch]$AllowMissingArchs)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path (Join-Path $repo 'src\core\version.h') -Pattern '#define TDPX_VERSION "([^"]+)"').Matches[0].Groups[1].Value
$name = "Teardown-PhysX-v$version"
$dist = Join-Path $repo 'dist'
$staging = Join-Path $repo 'build\release-staging'
$asi = Join-Path $repo 'build\TeardownPhysX.asi'
$gpu = Join-Path $repo 'build\PhysXGpu_64.dll'
# Ultimate ASI Loader v9.7.4 x64, shipped as dxgi.dll: the dinput8.dll of its release's Ultimate-ASI-Loader-NoPDB_x64.zip
$loader = Join-Path $repo 'third_party\uasi\v9.7.4\dinput8.dll'
$loaderSha256 = '031A3E5576D91DCE1E438D36B9A3D462C7334AB4791990A8FF1E3DDC0E132DAF'
foreach ($f in $asi, $gpu, $loader) { if (-not (Test-Path $f)) { throw "missing $f" } }

# archive name -> with the loader
$archives = [ordered]@{ "$name-plugin-only" = $false; "$name-with-Ultimate-ASI-Loader-v9.7.4" = $true }
foreach ($a in $archives.Keys) {
    foreach ($f in "$a.zip", "$a.zip.sha256") {
        if (Test-Path (Join-Path $dist $f)) { throw "dist\$f exists already: a published archive keeps its hash (delete it to rebuild)" }
    }
}

$fileVersion = (Get-Item $asi).VersionInfo.ProductVersion
if ($fileVersion -ne $version) { throw "build\TeardownPhysX.asi is version '$fileVersion', src\core\version.h says ${version}: rebuild" }
$loaderHash = (Get-FileHash $loader -Algorithm SHA256).Hash
if ($loaderHash -ne $loaderSha256) { throw "$loader is not the tested Ultimate ASI Loader v9.7.4 x64 (SHA-256 $loaderHash, not $loaderSha256)" }

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

# text files with Windows line endings, for Notepad
function Copy-Text($from, $to, $replace = @{}) {
    $t = [IO.File]::ReadAllText($from)
    foreach ($k in $replace.Keys) { $t = $t.Replace($k, $replace[$k]) }
    [IO.File]::WriteAllText($to, ($t -replace "`r?`n", "`r`n"), [Text.UTF8Encoding]::new($false))
}

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force $dist | Out-Null
foreach ($a in $archives.Keys) {
    $withLoader = $archives[$a]
    $stage = Join-Path $staging $(if ($withLoader) { 'with-loader' } else { 'plugin-only' })
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null
    if ($withLoader) { Copy-Item $loader (Join-Path $stage 'dxgi.dll') }
    Copy-Item $asi, $gpu $stage
    Copy-Text (Join-Path $PSScriptRoot 'package\TeardownPhysX.ini') (Join-Path $stage 'TeardownPhysX.ini')
    Copy-Text (Join-Path $PSScriptRoot 'package\README.txt') (Join-Path $stage 'README.txt') @{ '@VERSION@' = $version }
    Copy-Text (Join-Path $repo 'LICENSE') (Join-Path $stage 'licenses\Teardown-PhysX.txt')
    Copy-Text (Join-Path $repo 'third_party\physx-sdk\physx\LICENSE.md') (Join-Path $stage 'licenses\PhysX.txt')
    Copy-Text (Join-Path $repo 'third_party\minhook\LICENSE.txt') (Join-Path $stage 'licenses\MinHook.txt')
    if ($withLoader) {
        Copy-Text (Join-Path $PSScriptRoot 'package\licenses\Ultimate-ASI-Loader.txt') (Join-Path $stage 'licenses\Ultimate-ASI-Loader.txt')
    }

    # paths as the archive names them
    $files = @(Get-ChildItem $stage -Recurse -File | ForEach-Object {
        [pscustomobject]@{ Path = $_.FullName; Entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/') }
    } | Sort-Object Entry)
    $sums = $files | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.Path -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Entry }
    $sumsPath = Join-Path $stage 'SHA256SUMS.txt'
    [IO.File]::WriteAllText($sumsPath, ($sums -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)
    $files += [pscustomobject]@{ Path = $sumsPath; Entry = 'SHA256SUMS.txt' }

    # entries named with '/', as zip wants: Windows PowerShell's Compress-Archive writes '\', which unzip tools other
    # than Windows' own keep as part of the file name
    $zip = Join-Path $dist "$a.zip"
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $files) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.Path, $f.Entry, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    $zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash
    [IO.File]::WriteAllText("$zip.sha256", ('{0}  {1}' -f $zipHash.ToLowerInvariant(), "$a.zip") + "`r`n", [Text.Encoding]::ASCII)

    foreach ($f in $files) { '{0,12:N0}  {1}' -f (Get-Item $f.Path).Length, $f.Entry }
    '{0,12:N0}  {1} (SHA-256 {2})' -f (Get-Item $zip).Length, $zip, $zipHash
    ''
}

$symbols = Join-Path $dist "$name-symbols"
New-Item -ItemType Directory -Force $symbols | Out-Null
foreach ($ext in 'pdb', 'map') {
    $side = [IO.Path]::ChangeExtension($asi, $ext)
    if (Test-Path $side) { Copy-Item $side $symbols -Force }
}
