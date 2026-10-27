# One unattended game run with the TeardownPhysX tracer, as a shared-queue job step.
#   run_bench.ps1 -Label vertigo1_baseline -Replay vertigo1   (replay a recording from tools\record.ps1)
#                 [-ReRecord]  (also record the replay itself to <run>\rerecord.tdrec: a test of the recorder)
#                 [-ReplayFile <x.tdrec>]  (replay this file instead, with the recording's level and profile)
#                 [-RunDir <folder>]       (exact output folder instead of research\runs\<stamp>-<Label>)
#   run_bench.ps1 -Label lee_laser [-Mission lee_sandbox -Level lee.xml -Layers sandbox]
#                 [-Mod steam-2658944037 -Level vertigo.xml -Layers 'sunny sandbox tspawn'] (workshop level)
#                 [-Duration 45] [-HolesPerSecond 20] [-Smoke [-SmokeSeconds 40]] [-Set 'Bench.R0=2;Bench.Sound=0']
#                 [-TimeoutSec 420] [-GpuLog]
# -Smoke: no benchmark; start the game, let it sit in the main menu for SmokeSeconds, then kill it.
# -GpuLog: sample the GPU (nvidia-smi, no admin rights needed) every 100 ms into <run>\gpu.csv; seconds are
#          counted from the game's start (<run>\gpu_t0.txt), like the trace report's timeline.
# The game's LocalAppData is redirected to <run>\appdata (filled with copies of your options.xml and
# savegame.xml), so runs never write to your real profile. As a second safety layer the real profile is hashed
# (and copied to <run>\real-profile-before) before the run and checked afterwards; any change fails the run
# with exit code 90. The game is always killed, never closed gracefully: a normal exit saves mod lists.
# The plugin build is snapshotted into the run folder.
# Output: research\runs\<stamp>-<Label>\ (plugin.log, trace.bin, meta.json, game.log, summary.txt, ...).
param(
    [Parameter(Mandatory)][string]$Label,
    [string]$Replay = '',
    [switch]$ReRecord,
    [string]$ReplayFile = '',
    [string]$RunDir = '',
    [string]$Mod = '',
    [string]$Mission = 'lee_sandbox',
    [string]$Level = 'lee.xml',
    [string]$Layers = 'sandbox',
    [double]$Duration = 45,
    [double]$HolesPerSecond = 20,
    [switch]$Smoke,
    [int]$SmokeSeconds = 40,
    [string]$Set = '',
    [int]$TimeoutSec = 420,
    [switch]$GpuLog,
    # benchmarks: mods to enable in the sandbox's mod list, 'id;id' (e.g. 'local-zenitka-copy'); replays keep theirs
    [string]$Mods = '',
    [string]$GameDir = 'D:\Program Files\Games\Steam\steamapps\common\Teardown'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$python = 'C:\Python\312\python.exe'

if (Get-Process -Name teardown -ErrorAction SilentlyContinue) { throw 'teardown.exe is already running (someone playing?): not starting' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$run = if ($RunDir) { $RunDir } else { Join-Path $repo "research\runs\$stamp-$Label" }
if (Test-Path (Join-Path $run 'plugin.log')) { throw "$run already holds a run" }
New-Item -ItemType Directory -Force $run | Out-Null

# ---- real-profile guard: snapshot ------------------------------------------------------------------------
$real = Join-Path $env:LOCALAPPDATA 'Teardown'
$guardDir = Join-Path $run 'real-profile-before'
$before = @{}
if (Test-Path $real) {
    foreach ($f in Get-ChildItem $real -File -Recurse -Force) {
        $rel = $f.FullName.Substring($real.Length + 1)
        $before[$rel] = (Get-FileHash $f.FullName -Algorithm SHA256).Hash
        $dstFile = Join-Path $guardDir $rel
        New-Item -ItemType Directory -Force (Split-Path $dstFile) | Out-Null
        Copy-Item $f.FullName $dstFile
    }
}

# ---- sandboxed LocalAppData ---------------------------------------------------------------------------
# Benchmarks: copies of the user's settings and progress. Replays: the profile the recording started from.
$sandbox = Join-Path $run 'appdata'
$dst = Join-Path $sandbox 'Teardown'
New-Item -ItemType Directory -Force $dst | Out-Null
if ($Replay) {
    $recDir = Join-Path $repo "research\recordings\$Replay"
    $session = Get-Content (Join-Path $recDir 'session.json') -Raw | ConvertFrom-Json
    Copy-Item -Recurse (Join-Path $recDir 'appdata-start\Teardown\*') $dst
    $Mod = $session.mod; $Level = $session.level; $Layers = $session.layers; $Mission = $session.mission
} else {
    foreach ($f in 'options.xml', 'savegame.xml', '.guid') {
        if (Test-Path (Join-Path $real $f)) { Copy-Item (Join-Path $real $f) $dst }
    }
    if ($Mods) {  # the game's mod list format (modlists\index.xml + 1.xml), in the sandbox only
        $ml = Join-Path $dst 'modlists'
        New-Item -ItemType Directory -Force $ml | Out-Null
        @('<modlists version="2.1.0">', "`t<lastId>1</lastId>", "`t<modlist id=""1"">", "`t`t<name>Default</name>",
          "`t</modlist>", '</modlists>') | Set-Content -Encoding ASCII (Join-Path $ml 'index.xml')
        $ids = @($Mods -split ';' | Where-Object { $_ } | ForEach-Object { "`t`t<mod id=""$($_.Trim())""/>" })
        (@('<modlist version="2.1.0">', "`t<name>Default</name>", "`t<mods>") + $ids + @("`t</mods>", '</modlist>')) |
            Set-Content -Encoding ASCII (Join-Path $ml '1.xml')
    }
}

$mode = if ($Replay) { 'replay' } elseif ($Smoke) { 'none' } else { 'bench' }
$ini = [ordered]@{
    'Trace'   = [ordered]@{ Enabled = 1; RunDir = $run; Label = $Label }
    'Sandbox' = [ordered]@{ AppDataRoot = $sandbox }
    # optimized copies of mod files (research\mods); -Set 'Mods.Overrides=' runs the mods as published
    'Mods'    = [ordered]@{ Overrides = (Join-Path $repo 'research\mods\overrides') }
    'Session' = [ordered]@{ Mode = $mode }
    'Level'   = [ordered]@{ Mod = $Mod; Mission = $Mission; Level = $Level; Layers = $Layers }
    'Bench'   = [ordered]@{ Enabled = [int]($mode -eq 'bench'); DurationSeconds = $Duration;
                            HolesPerSecond = $HolesPerSecond; QuitWhenDone = 1 }
}
if ($Replay) {
    $file = if ($ReplayFile) { $ReplayFile } else { Join-Path $recDir 'input.tdrec' }
    $ini['Replay'] = [ordered]@{ File = $file; StopAtPause = 1 }
    if ($ReRecord) { $ini['Replay']['ReRecordFile'] = (Join-Path $run 'rerecord.tdrec') }
}
foreach ($kv in ($Set -split ';' | Where-Object { $_ -match '=' })) {
    $k, $v = $kv -split '=', 2
    $sec, $key = $k.Trim() -split '\.', 2
    if (-not $ini.Contains($sec)) { $ini[$sec] = [ordered]@{} }
    $ini[$sec][$key] = $v.Trim()
}
$iniPath = Join-Path $run 'TeardownPhysX.ini'
$lines = foreach ($sec in $ini.Keys) { "[$sec]"; foreach ($key in $ini[$sec].Keys) { "$key=$($ini[$sec][$key])" } }
$lines | Set-Content -Encoding ASCII $iniPath

Copy-Item (Join-Path $repo 'build\TeardownPhysX.asi') $run
if (Test-Path (Join-Path $repo 'build\TeardownPhysX.pdb')) { Copy-Item (Join-Path $repo 'build\TeardownPhysX.pdb') $run }
if (Test-Path (Join-Path $repo 'build\TeardownPhysX.map')) { Copy-Item (Join-Path $repo 'build\TeardownPhysX.map') $run }

& (Join-Path $PSScriptRoot 'install.ps1') -Asi (Join-Path $run 'TeardownPhysX.asi') -Ini $iniPath -GameDir $GameDir
$exitCode = 0
$gpu = $null
try {
    $env:TDPX_INI = $iniPath
    $env:SteamAppId = '1167630'
    $env:SteamGameId = '1167630'
    if ($GpuLog) {
        $gpu = Start-Process -FilePath 'nvidia-smi.exe' -WindowStyle Hidden -PassThru -ArgumentList @(
            '--query-gpu=timestamp,utilization.gpu,clocks.gr,power.draw', '--format=csv,nounits', '-lms', '100',
            '-f', (Join-Path $run 'gpu.csv'))
    }
    $t0 = Get-Date
    $t0.ToString('yyyy/MM/dd HH:mm:ss.fff') | Set-Content -Encoding ASCII (Join-Path $run 'gpu_t0.txt')
    # (not under Nsight Systems: its injection leaves MinHook no memory for trampolines, so the AppData redirect
    # fails; the plugin then ends the game at once, exit code 92)
    $proc = Start-Process -FilePath (Join-Path $GameDir 'teardown.exe') -WorkingDirectory $GameDir -PassThru
    Write-Output "game started, pid $($proc.Id)"
    $limit = if ($Smoke) { $SmokeSeconds } else { $TimeoutSec }
    while (((Get-Date) - $t0).TotalSeconds -lt $limit) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-Process -Name teardown -ErrorAction SilentlyContinue)) { break }
    }
    if (Get-Process -Name teardown -ErrorAction SilentlyContinue) {
        if (-not $Smoke) { Write-Output "timeout after $TimeoutSec s"; $exitCode = 124 }
        Get-Process -Name teardown -ErrorAction SilentlyContinue | Stop-Process -Force
    }
    Write-Output ("game ran {0:N0} s, exit code {1}" -f ((Get-Date) - $t0).TotalSeconds, $proc.ExitCode)
}
finally {
    if ($gpu) { Stop-Process -Id $gpu.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 1
    & (Join-Path $PSScriptRoot 'uninstall.ps1') -GameDir $GameDir
    $gameLog = Join-Path $dst 'log.txt'
    if (Test-Path $gameLog) { Copy-Item $gameLog (Join-Path $run 'game.log') }

    # ---- real-profile guard: verify ----------------------------------------------------------------------
    $changed = @()
    if (Test-Path $real) {
        foreach ($f in Get-ChildItem $real -File -Recurse -Force) {
            $rel = $f.FullName.Substring($real.Length + 1)
            $h = (Get-FileHash $f.FullName -Algorithm SHA256).Hash
            if (-not $before.ContainsKey($rel)) { $changed += "new: $rel" }
            elseif ($before[$rel] -ne $h) { $changed += "changed: $rel" }
        }
    }
    foreach ($rel in $before.Keys) { if (-not (Test-Path (Join-Path $real $rel))) { $changed += "removed: $rel" } }
    if ($changed.Count -gt 0) {
        Write-Output "WARNING: this run changed the real profile $real (pre-run copies in $guardDir):"
        $changed | ForEach-Object { Write-Output "  $_" }
        $exitCode = 90
    } else {
        Write-Output 'real profile untouched'
    }
}

Get-Content (Join-Path $run 'plugin.log') -ErrorAction SilentlyContinue | Select-Object -Last 30
if (Test-Path (Join-Path $run 'trace.bin')) {
    & $python -I (Join-Path $PSScriptRoot 'trace_report.py') $run
}
Write-Output "run folder: $run"
exit $exitCode
