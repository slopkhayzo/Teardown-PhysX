# Free play for trying out levels: Teardown opens at its main menu on a copy of your profile (as in record.ps1),
# with PhysX simulating the debris and the debris tool on (hold the middle mouse button to cut what you aim at into
# loose pieces). Start any levels from the menu, as many as you like; quit the game (Quit in the main or pause menu)
# to end the session. Nothing is recorded.
# The plugin logs each level you start (mission, path, layers, mod, size in voxels), where you cut from, and the
# frame rate every 10 s of play, so a level you liked can become a benchmark. The game's RAM and VRAM are read from
# outside it every 10 s too (memory.csv), and shown beside the frame rates at the end.
#
#   browse.ps1 -Queue                          (through the shared queue: the game opens when the job's turn comes)
#   browse.ps1 -Queue -Set 'PhysX.Gpu=0'       (other plugin settings, 'Section.Key=value;...' as in run_bench.ps1)
#   browse.ps1 -InQueue -Player -Asi research\builds\<name>\TeardownPhysX.asi
#       -Player: the plugin as players get it (tools\package\TeardownPhysX.ini on the plugin's own defaults: no debris
#       tool unless -Set 'Tool.Enabled=1', no mod overrides); its log goes to <game>\TeardownPhysX_logs, as for a
#       player, and is moved into the session folder afterwards. -Asi: the plugin build to play (profiles pin one).
#
# Output: research\browse\<date-time>\  plugin.log (the lines above are printed at the end), memory.csv,
#   TeardownPhysX.ini, ...
param(
    [switch]$Queue,
    [switch]$Force,
    [switch]$InQueue,
    [string]$Out = '',
    [string]$Set = '',
    [switch]$Player,
    [string]$Asi = '',
    [int]$TimeoutMinutes = 180,
    [string]$GameDir = 'D:\Program Files\Games\Steam\steamapps\common\Teardown'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$playerLogs = Join-Path $GameDir 'TeardownPhysX_logs'
if (-not $Asi) { $Asi = Join-Path $repo 'build\TeardownPhysX.asi' }
$queueCmd = 'D:\Githubs\_agents\queue_executor\queue.ps1'
if (-not $Out) { $Out = Join-Path $repo ('research\browse\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }

if ($Queue) {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $q = { param($s) "'" + ($s -replace "'", "''") + "'" }
    $cmd = "& $(& $q (Join-Path $PSScriptRoot 'browse.ps1')) -Out $(& $q $Out) -Set $(& $q $Set) " +
           "-Asi $(& $q $Asi) -GameDir $(& $q $GameDir) -InQueue$(if ($Player) { ' -Player' }); " + 'exit $LASTEXITCODE'
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
if ($Player -and (Test-Path $playerLogs)) { throw "$playerLogs exists (your own install?): not starting" }
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
if ($Player) {
    # the shipped settings file; then the profile copy, and the main menu's free play
    $sections = [ordered]@{}
    $sec = ''
    foreach ($line in Get-Content (Join-Path $PSScriptRoot 'package\TeardownPhysX.ini')) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $sec = $Matches[1]; $sections[$sec] = [ordered]@{}; continue }
        if ($line -match '^\s*;' -or $line -notmatch '=') { continue }
        $k, $v = $line -split '=', 2
        $sections[$sec][$k.Trim()] = $v.Trim()
    }
    $sections['Sandbox'] = [ordered]@{ AppDataRoot = $live }
    $sections['Session'] = [ordered]@{ Mode = 'browse' }
} else {
    $sections = [ordered]@{
        # no trace: an hour of it is gigabytes (plugin.log gets the frame rate every 10 s instead)
        'Trace'   = [ordered]@{ Enabled = 0; RunDir = $Out; Label = 'browse' }
        'Sandbox' = [ordered]@{ AppDataRoot = $live }
        'Mods'    = [ordered]@{ Overrides = (Join-Path $repo 'research\mods\overrides') }
        # you are at the keyboard: no hang watchdog (a minimized game may stop running frames). Defaults=dev: the
        # plugin's development defaults (the keys here override them).
        'Session' = [ordered]@{ Defaults = 'dev'; Mode = 'browse'; HangKillSeconds = 0 }
        'PhysX'   = [ordered]@{ Shadow = 3; Gpu = 1; LogEvery = 60 }
        'Fix'     = [ordered]@{ KeepDebris = 1 }
        'Tool'    = [ordered]@{ Enabled = 1 }
    }
}
foreach ($kv in ($Set -split ';' | Where-Object { $_ -match '=' })) {
    $k, $v = $kv -split '=', 2
    $sec, $key = $k.Trim() -split '\.', 2
    if (-not $sections.Contains($sec)) { $sections[$sec] = [ordered]@{} }
    $sections[$sec][$key] = $v.Trim()
}
$(foreach ($sec in $sections.Keys) { "[$sec]"; foreach ($key in $sections[$sec].Keys) { "$key=$($sections[$sec][$key])" } }) |
    Set-Content -Encoding ASCII $ini
Copy-Item $Asi $Out -Force
# a pinned build brings its own GPU module (research\builds\<name>\PhysXGpu_64.dll), else build\'s
$beside = Join-Path (Split-Path -Parent $Asi) 'PhysXGpu_64.dll'
$gpuDll = if (Test-Path $beside) { $beside } else { '' }
& (Join-Path $PSScriptRoot 'install.ps1') -Asi (Join-Path $Out 'TeardownPhysX.asi') -Ini $ini -GameDir $GameDir -GpuDll $gpuDll

# RAM and VRAM every 10 s while the game runs, from Windows' own counters (Task Manager's), into memory.csv:
#   game_ram_mb    the game's private memory (committed: Task Manager's commit size)
#   game_inram_mb  its working set, the part of it in RAM at that moment
#   free_ram_mb    the PC's available RAM
#   game_vram_mb   the game's dedicated GPU memory (Direct3D and PhysX's CUDA)
#   card_vram_mb   the card's memory in use by every program (as nvidia-smi gives it)
# The first row, 'before', is the PC before the game starts. The per-program GPU counter is wrong for some programs
# (one read 34 GB on a 24 GB card while the card's whole use was 2 GB), hence the card's too.
$memCsv = Join-Path $Out 'memory.csv'
function Get-CardVramMb {
    try {
        $a = Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory -ErrorAction Stop
        [math]::Round(($a | Measure-Object DedicatedUsage -Maximum).Maximum / 1MB)
    } catch { '' }
}
function Get-FreeRamMb {
    try { (Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory -ErrorAction Stop).AvailableMBytes } catch { '' }
}
function Write-MemorySample($game, [datetime]$start) {
    $now = Get-Date
    $ram = $inRam = $vram = ''
    try {
        $game.Refresh()
        $ram = [math]::Round($game.PrivateMemorySize64 / 1MB)
        $inRam = [math]::Round($game.WorkingSet64 / 1MB)
    } catch { }
    try {
        $g = Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -ErrorAction Stop |
            Where-Object { $_.Name -like "pid_$($game.Id)_*" }
        $vram = [math]::Round(($g | Measure-Object DedicatedUsage -Sum).Sum / 1MB)
    } catch { }
    Add-Content -Encoding ASCII $memCsv ('{0},{1:F0},{2},{3},{4},{5},{6}' -f $now.ToString('HH:mm:ss'),
        ($now - $start).TotalSeconds, $ram, $inRam, (Get-FreeRamMb), $vram, (Get-CardVramMb))
}

try {
    $env:TDPX_INI = $ini
    $env:SteamAppId = '1167630'
    $env:SteamGameId = '1167630'
    $toolOn = $sections.Contains('Tool') -and "$($sections['Tool']['Enabled'])" -eq '1'
    $tool = if ($toolOn) { ' Middle mouse: cut what you aim at.' } else { '' }
    # (the first counter query also starts Windows' counter provider, a few seconds, before the game rather than in it)
    Set-Content -Encoding ASCII $memCsv 'time,t_s,game_ram_mb,game_inram_mb,free_ram_mb,game_vram_mb,card_vram_mb'
    Add-Content -Encoding ASCII $memCsv ('before,,,,{0},,{1}' -f (Get-FreeRamMb), (Get-CardVramMb))
    Write-Output "Starting Teardown at its main menu.$tool Quit the game to end the session."
    $p = Start-Process -FilePath (Join-Path $GameDir 'teardown.exe') -WorkingDirectory $GameDir -PassThru
    $start = $p.StartTime
    $game = $p
    while ($game) {  # until no teardown.exe is left (one Steam starts in its place included)
        Write-MemorySample $game $start
        if ($game.WaitForExit(10000)) { $game = Get-Process -Name teardown -ErrorAction SilentlyContinue | Select-Object -First 1 }
    }
}
finally {
    & (Join-Path $PSScriptRoot 'uninstall.ps1') -GameDir $GameDir
    if ($Player -and (Test-Path $playerLogs)) {  # the plugin's own log folder, as a player's game gets it
        Get-ChildItem $playerLogs -File | ForEach-Object { Move-Item $_.FullName $Out -Force }
        Remove-Item $playerLogs -Recurse -Force
    }
}
$changed = @(foreach ($f in Get-ChildItem $real -File -Recurse -Force) {
    if ($before[$f.FullName] -ne (Get-FileHash $f.FullName).Hash) { $f.FullName }
})
if ($changed.Count) { Write-Output "WARNING: your real profile changed: $($changed -join ', ')" }
else { Write-Output 'real profile untouched' }

$mem = @(if (Test-Path $memCsv) { Import-Csv $memCsv })
$samples = @($mem | Where-Object { $_.t_s -ne '' })
$log = Join-Path $Out 'plugin.log'
if (Test-Path $log) {
    # the plugin's clock starts when it loads ('loaded 0.473 s after process start'), memory.csv's when the game starts
    $loaded = Select-String -Path $log -Pattern 'loaded ([\d.]+) s after process start' | Select-Object -First 1
    $offset = if ($loaded) { [double]$loaded.Matches[0].Groups[1].Value } else { 0.0 }
    $i = 0
    Write-Output 'Levels started, cuts, frame rates (with the game''s RAM and VRAM then):'
    Select-String -Path $log -Pattern '\] (level|frames|session): |tool: firing' | ForEach-Object {
        $line = '  ' + $_.Line
        if ($samples.Count -and $_.Line -match '^\s*([\d.]+) .*\] frames: ') {
            $t = [double]$Matches[1] + $offset  # the last sample before the end of the 10 s
            while ($i + 1 -lt $samples.Count -and [double]$samples[$i + 1].t_s -le $t) { $i++ }
            $m = $samples[$i]
            if ($m.game_ram_mb -ne '' -and $m.game_vram_mb -ne '') {
                $line += '; RAM {0:F1} GB, VRAM {1:F1} GB' -f ([double]$m.game_ram_mb / 1024), ([double]$m.game_vram_mb / 1024)
            }
        }
        $line
    }
}
if ($samples.Count) {
    $gb = { param($rows, $k, $most) $v = @($rows | Where-Object { $_.$k -ne '' } | ForEach-Object { [double]$_.$k })
            if (-not $v.Count) { return [double]::NaN }
            $s = $v | Measure-Object -Maximum -Minimum
            $(if ($most) { $s.Maximum } else { $s.Minimum }) / 1024 }
    $baseline = @($mem | Where-Object { $_.time -eq 'before' })
    Write-Output (('RAM and VRAM, every 10 s (memory.csv): the game used up to {0:F1} GB of RAM ({1:F1} GB of it in ' +
        'RAM at most); the PC''s free RAM went down to {2:F1} GB ({3:F1} GB before the game). The game''s VRAM went up ' +
        'to {4:F1} GB; the card''s, all programs together, up to {5:F1} GB ({6:F1} GB before the game).') -f
        (& $gb $samples 'game_ram_mb' $true), (& $gb $samples 'game_inram_mb' $true), (& $gb $samples 'free_ram_mb' $false),
        (& $gb $baseline 'free_ram_mb' $true), (& $gb $samples 'game_vram_mb' $true), (& $gb $samples 'card_vram_mb' $true),
        (& $gb $baseline 'card_vram_mb' $true))
}
Write-Output "Session folder: $Out"
