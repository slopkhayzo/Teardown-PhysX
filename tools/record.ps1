# Records a play session for exact replays: Teardown starts straight into the level, you play, then press Esc and
# choose "Main menu" (or quit the game): the recording is saved and the game closes by itself.
#
#   record.ps1 -Name vertigo1 -Mod steam-2658944037 -Level vertigo.xml -Layers 'sunny sandbox tspawn' -Mission de_vertigo -Queue
#   record.ps1 -Name lee1 -Queue                (Lee Chemicals sandbox)
#   record.ps1 -Name vertigo_px -Mod ... -Set 'PhysX.Shadow=3' -Queue   (play with PhysX simulating the debris)
# Without -Name the recording is named <preset or "rec">-<date>-<time>.
# The user starts these from the queue app instead (profiles\teardown-physx in the queue folder), which runs them as
# queue jobs with -InQueue.
#
# -Set: plugin settings for the session ('Section.Key=value;...', as in run_bench.ps1). They are stored in
#       session.json; replays use the plugin's defaults unless run_bench.ps1 gets the same -Set.
# -Queue: run as a job of the shared queue (D:\Githubs\_agents\queue_executor) and wait for it, so that no other
#         agent's live test runs while you play. The game opens when the job's turn comes; click into its window.
# Without -Queue the game starts at once (refused while a queued job is running, unless -Force).
# The game runs on a copy of your profile (options, progress, mod lists: your enabled mods are active), never on the
# real one; the copy at the start is kept with the recording so every replay starts from the same profile.
# Output: research\recordings\<Name>\  input.tdrec, session.json, appdata-start\ (profile), track.csv, plugin.log...
# Replay it unattended through the queue: tools\run_bench.ps1 -Label <x> -Replay <Name>, or tools\replay_check.ps1.
#
# -Preset: a heavy level with the settings for a stress session (PhysX on the GPU, KeepDebris, the debris tool, the
# stack sampler and thread census, no hang watchdog); -Set adds to them or overrides them:
#   record.ps1 -Name acacia1 -Preset acacia -Queue     (Acacia Apartments, all towers)
#   record.ps1 -Name plaza1 -Preset plaza -Queue       (Voxel Plaza with your menu choices; the debris tool is on
#                                                       the mouse's back side button: the map's own "Make a Hole"
#                                                       power is on the middle button)
#   record.ps1 -Name russian1 -Preset russian -Queue   (Russian Town 3)
#   record.ps1 -Name volkograd1 -Preset volkograd -Queue   (Volkograd town winter: the biggest piles so far)
param(
    [string]$Name = '',          # default: <preset or "rec">-<date>-<time>
    [string]$Preset = '',
    [string]$Mod = '',
    [string]$Level = 'lee.xml',
    [string]$Layers = 'sandbox',
    [string]$Mission = 'lee_sandbox',
    [int]$Seed = 1,
    [switch]$Queue,
    [switch]$Force,
    [switch]$InQueue,
    [int]$StopAfterFrames = 0,   # tests: the recording ends by itself after this many frames
    [string]$Set = '',
    [string]$Asi = '',           # the plugin build to play (default: build\TeardownPhysX.asi; profiles pin one)
    [string]$GameDir = 'D:\Program Files\Games\Steam\steamapps\common\Teardown'
)
$ErrorActionPreference = 'Stop'
if (-not $Name) { $Name = "$(if ($Preset) { $Preset } else { 'rec' })-$(Get-Date -Format 'yyyyMMdd-HHmm')" }
if ($Preset) {
    $stress = 'PhysX.Shadow=3;PhysX.Gpu=1;Fix.KeepDebris=1;Tool.Enabled=1;Trace.Sampler=1;Trace.Threads=1;Session.HangKillSeconds=0'
    $presets = @{
        acacia  = @{ Mod = 'steam-2498343049'; Level = 'map.xml'; Mission = ''
                     Layers = 'tower-brick tower-brick-furniture tower-cladding tower-cladding-furniture tower-wood tower-wood-furniture'
                     Set = $stress }
        plaza   = @{ Mod = 'steam-2430961153'; Level = 'level.xml'; Mission = 'level'
                     Layers = 'Storm buildSolid+d street_props VPnpcs carsonroad parkedcars trees ultra underground_cleaner'
                     Set = "$stress;Tool.Key=xbutton1" }
        russian = @{ Mod = 'steam-2453693772'; Level = 'main.xml'; Mission = 'main'; Layers = ''; Set = $stress }
        volkograd = @{ Mod = 'steam-2959348276'; Level = 'main.xml'; Mission = 'main'; Layers = ''; Set = $stress }
    }
    if (-not $presets.ContainsKey($Preset)) { throw "unknown preset $Preset (known: $($presets.Keys -join ', '))" }
    $p = $presets[$Preset]
    $Mod = $p.Mod; $Level = $p.Level; $Layers = $p.Layers; $Mission = $p.Mission
    $Set = if ($Set) { "$($p.Set);$Set" } else { $p.Set }  # later keys win
}
$repo = Split-Path -Parent $PSScriptRoot
$python = 'C:\Python\312\python.exe'
$queueCmd = 'D:\Githubs\_agents\queue_executor\queue.ps1'
$rec = Join-Path $repo "research\recordings\$Name"
if ((Test-Path (Join-Path $rec 'input.tdrec')) -and -not $Force) {
    throw "$rec already holds a recording; pick another name or pass -Force to overwrite"
}

if ($Queue) {
    New-Item -ItemType Directory -Force $rec | Out-Null
    $q = { param($s) "'" + ($s -replace "'", "''") + "'" }
    $cmd = "& $(& $q (Join-Path $PSScriptRoot 'record.ps1')) -Name $(& $q $Name) -Mod $(& $q $Mod) " +
           "-Level $(& $q $Level) -Layers $(& $q $Layers) -Mission $(& $q $Mission) -Seed $Seed " +
           "-StopAfterFrames $StopAfterFrames -Set $(& $q $Set) -Asi $(& $q $Asi) -InQueue -Force; " + 'exit $LASTEXITCODE'
    $job = [ordered]@{
        name = "Teardown play session (user at the keyboard): record $Name"
        owner = 'Teardown-PhysX'
        workingDirectory = $repo
        timeoutMinutes = 120
        continueOnError = $false
        steps = @(@{ powershell = $cmd })
    }
    $jobFile = Join-Path $rec 'record-job.json'
    $job | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $jobFile
    Write-Output "Queued. Teardown opens when the job's turn comes (other agents' tests may be ahead); keep an eye on the screen."
    & $queueCmd submit -JobFile $jobFile -Wait -WaitTimeoutMinutes 240 -Tail 12
    exit $LASTEXITCODE
}

if (Get-Process -Name teardown -ErrorAction SilentlyContinue) { throw 'Close Teardown first.' }
if (-not $InQueue -and -not $Force -and ((& $queueCmd status) -match '^RUNNING')) {
    throw "Another agent's queued test is running right now (queue.ps1 status); recording would disturb it. Use -Queue."
}
New-Item -ItemType Directory -Force $rec | Out-Null

# Profile copy: settings, progress and mod lists (so the same mods are enabled). The real profile is not touched.
$real = Join-Path $env:LOCALAPPDATA 'Teardown'
$start = Join-Path $rec 'appdata-start\Teardown'
$live = Join-Path $rec 'appdata'
Remove-Item -Recurse -Force $start, $live -ErrorAction SilentlyContinue
foreach ($f in 'input.tdrec', 'track.csv', 'plugin.log', 'trace.bin', 'census.csv', 'meta.json', 'session.json') {
    Remove-Item -Force (Join-Path $rec $f) -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force (Join-Path $start 'modlists') | Out-Null
foreach ($f in 'options.xml', 'savegame.xml', 'mods.xml', '.guid') {
    if (Test-Path (Join-Path $real $f)) { Copy-Item (Join-Path $real $f) $start }
}
if (Test-Path (Join-Path $real 'modlists')) { Copy-Item (Join-Path $real 'modlists\*') (Join-Path $start 'modlists') }
# The game reads <LocalAppData>\Teardown, and LocalAppData is redirected to $live.
New-Item -ItemType Directory -Force (Join-Path $live 'Teardown') | Out-Null
Copy-Item -Recurse (Join-Path $start '*') (Join-Path $live 'Teardown')
$before = @{}
foreach ($f in Get-ChildItem $real -File -Recurse -Force) { $before[$f.FullName] = (Get-FileHash $f.FullName).Hash }

$ini = Join-Path $rec 'TeardownPhysX.ini'
$sections = [ordered]@{
    'Trace'   = [ordered]@{ Enabled = 1; RunDir = $rec; Label = "record-$Name" }
    'Sandbox' = [ordered]@{ AppDataRoot = $live }
    # optimized copies of mod files (research\mods); -Set 'Mods.Overrides=' plays the mods as published
    'Mods'    = [ordered]@{ Overrides = (Join-Path $repo 'research\mods\overrides') }
    'Session' = [ordered]@{ Defaults = 'dev'; Mode = 'record' }  # Defaults=dev: the plugin's development defaults
    'Level'   = [ordered]@{ Mod = $Mod; Level = $Level; Layers = $Layers; Mission = $Mission }
    'Record'  = [ordered]@{ File = (Join-Path $rec 'input.tdrec'); QuitAfter = 1; Seed = $Seed;
                            StopAfterFrames = $StopAfterFrames }
}
foreach ($kv in ($Set -split ';' | Where-Object { $_ -match '=' })) {
    $k, $v = $kv -split '=', 2
    $sec, $key = $k.Trim() -split '\.', 2
    if (-not $sections.Contains($sec)) { $sections[$sec] = [ordered]@{} }
    $sections[$sec][$key] = $v.Trim()
}
$(foreach ($sec in $sections.Keys) { "[$sec]"; foreach ($key in $sections[$sec].Keys) { "$key=$($sections[$sec][$key])" } }) |
    Set-Content -Encoding ASCII $ini
if (-not $Asi) { $Asi = Join-Path $repo 'build\TeardownPhysX.asi' }
Copy-Item $Asi $rec -Force
# a pinned build brings its own GPU module (research\builds\<name>\PhysXGpu_64.dll), else build\'s
$beside = Join-Path (Split-Path -Parent $Asi) 'PhysXGpu_64.dll'
$gpuDll = if (Test-Path $beside) { $beside } else { '' }
& (Join-Path $PSScriptRoot 'install.ps1') -Asi (Join-Path $rec 'TeardownPhysX.asi') -Ini $ini -GameDir $GameDir -GpuDll $gpuDll
try {
    $env:TDPX_INI = $ini
    $env:SteamAppId = '1167630'
    $env:SteamGameId = '1167630'
    Write-Output 'Starting Teardown: it loads the level by itself (input is ignored until the level runs).'
    Write-Output 'Play; when you are done press Esc -> Main menu. The game then saves the recording and closes.'
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

$tdrec = Join-Path $rec 'input.tdrec'
if (-not (Test-Path $tdrec)) { throw "no recording was written; see $rec\plugin.log" }
[pscustomobject]@{
    name = $Name; created = (Get-Date -Format s); mod = $Mod; level = $Level; layers = $Layers; mission = $Mission
    seed = $Seed; settings = $Set
    game_exe_sha256 = (Get-FileHash (Join-Path $GameDir 'teardown.exe')).Hash
    plugin_sha256 = (Get-FileHash (Join-Path $rec 'TeardownPhysX.asi')).Hash
} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $rec 'session.json')
& $python -I (Join-Path $PSScriptRoot 'recording.py') info $tdrec
Write-Output "Saved: $rec"
