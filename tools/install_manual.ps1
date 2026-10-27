# Installs the TeardownPhysX tracer for a session you play yourself (launch Teardown through Steam as usual).
# Trace only: no benchmark, no AppData sandbox, the plugin writes nothing into the game or your profile.
# Each launch writes research\runs\<stamp>-manual\ (plugin.log, trace.bin, meta.json); afterwards:
#   python tools\trace_report.py research\runs\<stamp>-manual
# Remove with tools\uninstall.ps1 when done.
#   install_manual.ps1 [-Label manual] [-GameDir <dir>]
param(
    [string]$Label = 'manual',
    [string]$GameDir = 'D:\Program Files\Games\Steam\steamapps\common\Teardown'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (Get-Process -Name teardown -ErrorAction SilentlyContinue) { throw 'Close Teardown first.' }
$staging = Join-Path $repo 'research\runs\_manual_install'
New-Item -ItemType Directory -Force $staging | Out-Null
Copy-Item (Join-Path $repo 'build\TeardownPhysX.asi') $staging -Force
$ini = Join-Path $staging 'TeardownPhysX.ini'
@(
    '[Trace]'
    'Enabled=1'
    "OutDir=$(Join-Path $repo 'research\runs')"
    "Label=$Label"
    '[Bench]'
    'Enabled=0'
) | Set-Content -Encoding ASCII $ini
& (Join-Path $PSScriptRoot 'install.ps1') -Asi (Join-Path $staging 'TeardownPhysX.asi') -Ini $ini -GameDir $GameDir
Write-Output 'Tracer installed. Start Teardown from Steam, play, quit. Then run tools\uninstall.ps1.'
