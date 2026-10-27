# Restores the Default mod list that an early test launch (outside Steam, without the sandbox) emptied on
# 2026-10-18: it re-enables Explosive Pack (steam-2604914470) and Gluon Plasma Diffuser (steam-2972672998),
# the two mods your 18:38 session log showed as active, and puts savegame.xml's launch counter back to 107
# (the two test launches had raised it to 109).
# Same effect as re-enabling both mods in Teardown's Mods menu. Run it only with Teardown closed.
# The current files are copied to research\backups\appdata-<stamp>-before-restore\ first.
param([switch]$KeepStartCount)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name teardown -ErrorAction SilentlyContinue) { throw 'Close Teardown first.' }
$repo = Split-Path -Parent $PSScriptRoot
$td = Join-Path $env:LOCALAPPDATA 'Teardown'
$backup = Join-Path $repo ("research\backups\appdata-{0}-before-restore" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force (Join-Path $backup 'modlists') | Out-Null
Copy-Item (Join-Path $td '*.xml') $backup
Copy-Item (Join-Path $td 'modlists\*') (Join-Path $backup 'modlists')

$modlist = Join-Path $td 'modlists\1.xml'
$raw = [IO.File]::ReadAllText($modlist)
$nl = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }
$text = @(
    '<modlist version="2.1.0">'
    "`t<name>Default</name>"
    "`t<mods>"
    "`t`t<mod id=`"steam-2604914470`"/>"
    "`t`t<mod id=`"steam-2972672998`"/>"
    "`t</mods>"
    '</modlist>'
) -join $nl
[IO.File]::WriteAllText($modlist, $text + $nl)
Write-Output "restored $modlist"

if (-not $KeepStartCount) {
    $save = Join-Path $td 'savegame.xml'
    $s = [IO.File]::ReadAllText($save)
    if ($s -match '<startcount value="109"/>') {
        [IO.File]::WriteAllText($save, $s.Replace('<startcount value="109"/>', '<startcount value="107"/>'))
        Write-Output 'startcount 109 -> 107'
    } else {
        Write-Output 'startcount is not 109 (you have launched the game since): left as is'
    }
}
Write-Output "backup of the previous files: $backup"
