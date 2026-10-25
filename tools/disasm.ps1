# Disassembles game functions by mangled-name substring, annotating call/jmp/lea targets with symbol names.
# Needs LLVM (llvm-objdump) and a publics dump of the game's PDB. The dump is game-derived: keep it local.
#   llvm-pdbutil dump -publics "<game>\DoorKickers2.pdb" > publics.txt
# In the loader repo, publics.txt and the output go in re\ (git-ignored); elsewhere, next to this script.
# usage: disasm.ps1 -Names '?UpdateCamera@GameClient', '?SetDefaults@Camera' [-GameDir ...] [-Publics ...] [-OutDir ...]
param(
    [Parameter(Mandatory)][string[]]$Names,
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\DoorKickers2',
    [string]$Publics,
    [string]$Objdump,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$local = Join-Path $PSScriptRoot '..\re'
$local = if (Test-Path $local) { (Resolve-Path $local).Path } else { $PSScriptRoot }
if (-not $Publics) { $Publics = Join-Path $local 'publics.txt' }
if (-not $OutDir) { $OutDir = $local }
$exe = Join-Path $GameDir 'DoorKickers2.exe'
if (-not (Test-Path $exe)) { throw "no DoorKickers2.exe in $GameDir (pass -GameDir)" }
if (-not (Test-Path $Publics)) { throw "no ${Publics}: dump it with llvm-pdbutil dump -publics (see the top of this script)" }
if (-not $Objdump) {
    $cmd = Get-Command llvm-objdump -ErrorAction SilentlyContinue
    $Objdump = if ($cmd) { $cmd.Source } else { 'C:\Program Files\LLVM\bin\llvm-objdump.exe' }
}
if (-not (Test-Path $Objdump)) { throw "llvm-objdump not found (install LLVM or pass -Objdump)" }

# Section addresses come from the exe's headers: publics give <section>:<offset>, objdump uses the preferred image
# base, and both change with game builds.
function Get-Sections([string]$path) {
    $buf = New-Object byte[] 4096
    $fs = [IO.File]::OpenRead($path)
    try { [void]$fs.Read($buf, 0, $buf.Length) } finally { $fs.Dispose() }
    $pe = [BitConverter]::ToInt32($buf, 0x3C)
    if ([BitConverter]::ToUInt32($buf, $pe) -ne 0x4550) { throw "$path is not a PE file" }
    $count = [BitConverter]::ToUInt16($buf, $pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($buf, $pe + 20)
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($buf, $optional) -ne 0x20B) { throw "$path is not a 64-bit PE file" }
    $imageBase = [BitConverter]::ToUInt64($buf, $optional + 24)
    $sections = @{}
    for ($i = 0; $i -lt $count; $i++) {
        $s = $optional + $optionalSize + 40 * $i
        $sections[$i + 1] = [pscustomobject]@{
            Name = [Text.Encoding]::ASCII.GetString($buf, $s, 8).TrimEnd([char]0)
            Va   = [uint64]$imageBase + [BitConverter]::ToUInt32($buf, $s + 12)
            Code = ([BitConverter]::ToUInt32($buf, $s + 36) -band 0x20) -ne 0 # IMAGE_SCN_CNT_CODE
        }
    }
    $sections
}
$sections = Get-Sections $exe

if (-not $script:syms -or $script:symsFrom -ne "$Publics|$exe") {
    $lines = [IO.File]::ReadAllLines($Publics)
    $list = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $lines.Length - 1; $i++) {
        if ($lines[$i] -match 'S_PUB32 \[size = \d+\] `(.+)`$') {
            $name = $Matches[1]
            if ($lines[$i + 1] -match 'addr = (\d{4}):(\d+)') {
                $sec = [int]$Matches[1]
                if ($sections.ContainsKey($sec)) {
                    $list.Add([pscustomobject]@{ Name = $name; Addr = [uint64]($sections[$sec].Va + [uint64]$Matches[2]); Sec = $sec })
                }
            }
        }
    }
    $script:syms = $list | Sort-Object Addr
    $script:byAddr = @{}
    foreach ($s in $script:syms) { if (-not $script:byAddr.ContainsKey($s.Addr)) { $script:byAddr[$s.Addr] = $s.Name } }
    $script:symsFrom = "$Publics|$exe"
}
$text = @($script:syms | Where-Object { $sections[$_.Sec].Code })

foreach ($n in $Names) {
    $idx = -1
    for ($i = 0; $i -lt $text.Count; $i++) { if ($text[$i].Name -like "*$n*") { $idx = $i; break } }
    if ($idx -lt 0) { Write-Warning "no symbol matching $n"; continue }
    $start = $text[$idx].Addr
    $j = $idx + 1; while ($j -lt $text.Count -and $text[$j].Addr -eq $start) { $j++ }
    $stop = $text[$j].Addr
    $asm = & $Objdump -d --no-show-raw-insn --start-address=$start --stop-address=$stop $exe
    $annotated = foreach ($l in $asm) {
        if ($l -match '0x([0-9a-f]{9,16})\b') {
            $a = [Convert]::ToUInt64($Matches[1], 16)
            if ($script:byAddr.ContainsKey($a)) { "$l    ; $($script:byAddr[$a])"; continue }
        }
        $l
    }
    $file = Join-Path $OutDir (($text[$idx].Name -replace '[^A-Za-z0-9_]', '_').Substring(0, [Math]::Min(60, $text[$idx].Name.Length)) + '.asm')
    Set-Content $file $annotated
    Write-Host ("{0}  0x{1:X}-0x{2:X}  {3} lines -> {4}" -f $text[$idx].Name, $start, $stop, $annotated.Count, $file)
}
