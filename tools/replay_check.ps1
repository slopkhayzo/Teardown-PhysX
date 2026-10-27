# Checks a recording end to end, as one shared-queue job step:
#   1. replay it and record the replay itself (run A: track.csv + rerecord.tdrec, written by the recorder code);
#   2. replay A's re-recording (run B), which compares itself frame by frame with A's checkpoints;
#   3. compare A with the original recording (how faithful a replay is) and A with B (run-to-run determinism).
#   replay_check.ps1 -Replay synth_lee [-Name check1] [-Set 'Fix.TouchCache=1'] [-TimeoutSec 900]
# Output: research\runs\<stamp>-<Name>-a\ and ...-b\ (each a normal run_bench.ps1 run folder).
param(
    [Parameter(Mandatory)][string]$Replay,
    [string]$Name = 'check',
    [string]$Set = '',
    [int]$TimeoutSec = 900
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$python = 'C:\Python\312\python.exe'
$bench = Join-Path $PSScriptRoot 'run_bench.ps1'
$tool = Join-Path $PSScriptRoot 'recording.py'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$a = Join-Path $repo "research\runs\$stamp-$Name-a"
$b = Join-Path $repo "research\runs\$stamp-$Name-b"
$recording = Join-Path $repo "research\recordings\$Replay\input.tdrec"

& $bench -Label "$Name-a" -Replay $Replay -ReRecord -RunDir $a -Set $Set -TimeoutSec $TimeoutSec | Select-Object -Last 12
$codeA = $LASTEXITCODE
if (-not (Test-Path (Join-Path $a 'rerecord.tdrec'))) { Write-Output 'run A wrote no re-recording'; exit 2 }
& $bench -Label "$Name-b" -Replay $Replay -ReplayFile (Join-Path $a 'rerecord.tdrec') -RunDir $b -Set $Set `
    -TimeoutSec $TimeoutSec | Select-Object -Last 12
$codeB = $LASTEXITCODE

Write-Output '=== the re-recording made by run A'
& $python -I $tool info (Join-Path $a 'rerecord.tdrec')
$synthetic = (& $python -I $tool info $recording | Select-Object -First 1) -match '\(synthetic\)'
if (-not $synthetic) {
    Write-Output '=== original recording vs run A (replay fidelity)'
    & $python -I $tool compare $recording $a
}
Write-Output '=== run A vs run B (determinism; B replayed the file the recorder wrote during A)'
& $python -I $tool compare $a $b
Write-Output '=== run A, live comparison with the recording'
Get-Content (Join-Path $a 'plugin.log') | Select-String 'replay: (camera|body|main|player|done|STOPPED)' | ForEach-Object { $_.Line }
Write-Output '=== run B, live comparison with A'
Get-Content (Join-Path $b 'plugin.log') | Select-String 'replay:|determinism:|rand\(\) off' | ForEach-Object { $_.Line }
Write-Output "runs: $a  $b"
if ($codeA -ne 0 -or $codeB -ne 0) { exit [Math]::Max($codeA, $codeB) }
exit 0
