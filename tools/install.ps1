# Dev install: dry-runs the plugin with symtest, then copies the mod (mod\ + the DLL in native\) to
# %LOCALAPPDATA%\KillHouseGames\DoorKickers2\mods_upload\<name>\ (earlier files stay unless overwritten). Mods there
# load without the Workshop prompt. Enable it once in the Mods menu, then watch dk2ml.log in the game folder. Close the
# game first: it locks the DLL.
#   -GameDir  the game folder;  -SymTest  symtest.exe (default: the one next to this script, as in the template zip)
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\DoorKickers2',
    [string]$SymTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent # the project root, one up from tools\
$name = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\((\w+)').Groups[1].Value
$dll = Join-Path $root "build\$name.dll"
if (-not (Test-Path $dll)) { throw 'Build first: .\tools\build.ps1' }
if (Get-Process DoorKickers2 -ErrorAction SilentlyContinue) { throw 'Close Door Kickers 2 first (it locks the DLL).' }

# Dry run: DK2ML_PluginInit against the real DoorKickers2.pdb, nothing hooked. Catches wrong names, and calls the real
# loader would refuse (exit code 100).
if (-not $SymTest) { $SymTest = Join-Path $PSScriptRoot 'symtest.exe' }
if (Test-Path $SymTest) {
    & $SymTest $GameDir $dll | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "symtest dry run failed (exit code $LASTEXITCODE): see above" }
} else {
    Write-Warning 'symtest.exe not found (pass -SymTest): skipping the dry run'
}

$target = Join-Path $env:LOCALAPPDATA "KillHouseGames\DoorKickers2\mods_upload\$name"
New-Item -ItemType Directory -Force (Join-Path $target 'native') | Out-Null
Copy-Item (Join-Path $root 'mod\*') $target -Recurse -Force
Copy-Item $dll (Join-Path $target 'native') -Force
Write-Host "Installed to $target. Enable it in the game's Mods menu (once), then check $GameDir\dk2ml.log."
