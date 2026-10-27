# Free play for trying out levels: Teardown opens at its main menu on a copy of your profile (as in record.ps1),
# with PhysX simulating the debris and the debris tool on (hold the middle mouse button to cut what you aim at into
# loose pieces). Start any levels from the menu, as many as you like; quit the game (Quit in the main or pause menu)
# to end the session. Nothing is recorded.
# The plugin logs each level you start (mission, path, layers, mod, size in voxels), where you cut from, and the
# frame rate every 10 s of play, so a level you liked can become a benchmark.
#
#   browse.ps1 -Queue                          (through the shared queue: the game opens when the job's turn comes)
#   browse.ps1 -Queue -Set 'PhysX.Gpu=0'       (other plugin settings, 'Section.Key=value;...' as in run_bench.ps1)
#
# Output: research\browse\<date-time>\  plugin.log (the lines above are printed at the end), TeardownPhysX.ini, ...
param(
    [switch]$Queue,
    [switch]$Force,
    [switch]$InQueue,
    [string]$Out = '',
    [string]$Set = '',
    [int]$TimeoutMinutes = 180,
    [string]$GameDir = 'D:\Program Files\Games\Steam\steamapps\common\Teardown'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$queueCmd = 'D:\Githubs\_agents\queue_executor\queue.ps1'
if (-not $Out) { $Out = Join-Path $repo ('research\browse\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }

if ($Queue) {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $q = { param($s) "'" + ($s -replace "'", "''") + "'" }
    $cmd = "& $(& $q (Join-Path $PSScriptRoot 'browse.ps1')) -Out $(& $q $Out) -Set $(& $q $Set) " +
           "-GameDir $(& $q $GameDir) -InQueue; " + 'exit $LASTEXITCODE'
    $job = [ordered]@{
        name = 'Teardown play session (user at the keyboard): browse levels'
        owner = 'Teardown-PhysX'
        workingDirectory = $repo
        timeoutMinutes = $TimeoutMinutes
        continueOnError = $false
        steps = @(@{ powershell = $cmd })
    }
    $jobFile = Join-Path $Out 'browse-job.json'
    $job | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $jobFile
    Write-Output "Queued. Teardown opens at its main menu when the job's turn comes (other agents' tests may be ahead)."
    & $queueCmd submit -JobFile $jobFile -Wait -WaitTimeoutMinutes ($TimeoutMinutes + 60) -Tail 40
    exit $LASTEXITCODE
}

if (Get-Process -Name teardown -ErrorAction SilentlyContinue) { throw 'Close Teardown first.' }
if (-not $InQueue -and -not $Force -and ((& $queueCmd status) -match '^RUNNING')) {
    throw "Another agent's queued test is running right now (queue.ps1 status); playing would disturb it. Use -Queue."
}
New-Item -ItemType Directory -Force $Out | Out-Null

# Profile copy: settings, progress and mod lists (so the same mods are enabled). The real profile is not touched.
$real = Join-Path $env:LOCALAPPDATA 'Teardown'
$live = Join-Path $Out 'appdata'
Remove-Item -Recurse -Force $live -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $live 'Teardown\modlists') | Out-Null
foreach ($f in 'options.xml', 'savegame.xml', 'mods.xml', '.guid') {
    if (Test-Path (Join-Path $real $f)) { Copy-Item (Join-Path $real $f) (Join-Path $live 'Teardown') }
}
if (Test-Path (Join-Path $real 'modlists')) {
    Copy-Item (Join-Path $real 'modlists\*') (Join-Path $live 'Teardown\modlists')
}
$before = @{}
foreach ($f in Get-ChildItem $real -File -Recurse -Force) { $before[$f.FullName] = (Get-FileHash $f.FullName).Hash }

$ini = Join-Path $Out 'TeardownPhysX.ini'
$sections = [ordered]@{
    # no trace: an hour of it is gigabytes (plugin.log gets the frame rate every 10 s instead)
    'Trace'   = [ordered]@{ Enabled = 0; RunDir = $Out; Label = 'browse' }
    'Sandbox' = [ordered]@{ AppDataRoot = $live }
    'Mods'    = [ordered]@{ Overrides = (Join-Path $repo 'research\mods\overrides') }
    # you are at the keyboard: no hang watchdog (a minimized game may stop running frames)
    'Session' = [ordered]@{ Mode = 'browse'; HangKillSeconds = 0 }
    'PhysX'   = [ordered]@{ Shadow = 3; Gpu = 1 }
    'Fix'     = [ordered]@{ KeepDebris = 1 }
    'Tool'    = [ordered]@{ Enabled = 1 }
}
foreach ($kv in ($Set -split ';' | Where-Object { $_ -match '=' })) {
    $k, $v = $kv -split '=', 2
    $sec, $key = $k.Trim() -split '\.', 2
    if (-not $sections.Contains($sec)) { $sections[$sec] = [ordered]@{} }
    $sections[$sec][$key] = $v.Trim()
}
$(foreach ($sec in $sections.Keys) { "[$sec]"; foreach ($key in $sections[$sec].Keys) { "$key=$($sections[$sec][$key])" } }) |
    Set-Content -Encoding ASCII $ini
Copy-Item (Join-Path $repo 'build\TeardownPhysX.asi') $Out -Force
& (Join-Path $PSScriptRoot 'install.ps1') -Asi (Join-Path $Out 'TeardownPhysX.asi') -Ini $ini -GameDir $GameDir
try {
    $env:TDPX_INI = $ini
    $env:SteamAppId = '1167630'
    $env:SteamGameId = '1167630'
    Write-Output 'Starting Teardown at its main menu. Middle mouse: cut what you aim at. Quit the game to end the session.'
    $p = Start-Process -FilePath (Join-Path $GameDir 'teardown.exe') -WorkingDirectory $GameDir -PassThru
    $p.WaitForExit()
    while (Get-Process -Name teardown -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 1 }
}
finally {
    & (Join-Path $PSScriptRoot 'uninstall.ps1') -GameDir $GameDir
}
$changed = @(foreach ($f in Get-ChildItem $real -File -Recurse -Force) {
    if ($before[$f.FullName] -ne (Get-FileHash $f.FullName).Hash) { $f.FullName }
})
if ($changed.Count) { Write-Output "WARNING: your real profile changed: $($changed -join ', ')" }
else { Write-Output 'real profile untouched' }

$log = Join-Path $Out 'plugin.log'
if (Test-Path $log) {
    Write-Output 'Levels started, cuts, frame rates:'
    Select-String -Path $log -Pattern '\] (level|frames|session): |tool: firing' | ForEach-Object { '  ' + $_.Line }
}
Write-Output "Session folder: $Out"
