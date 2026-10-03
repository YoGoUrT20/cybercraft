# Rebuilds red4ext\archive\CyberCraft.archive from red4ext\archive\source (WolvenKit JSON, one file per
# resource, laid out by its game path) with the WolvenKit console, from
# https://github.com/WolvenKit/WolvenKit/releases (WolvenKit.Console-<version>.zip):
#
#   powershell -ExecutionPolicy Bypass -File tools\build_archive.ps1 -WolvenKit <folder with WolvenKit.CLI.exe>
#
# The archive holds cybercraft\empty.ent: an entity with nothing in it, which scripts\CyberCraft.reds
# spawns and gives a light or a collider.
param([Parameter(Mandatory = $true)][string]$WolvenKit)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $WolvenKit "WolvenKit.CLI.exe"
if (-not (Test-Path $cli)) { throw "no WolvenKit.CLI.exe in $WolvenKit" }
$source = "$root\red4ext\archive\source"
$work = Join-Path ([IO.Path]::GetTempPath()) "cybercraft-archive"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$pack = "$work\CyberCraft"
Get-ChildItem $source -Recurse -Filter *.json | ForEach-Object {
    $relative = $_.DirectoryName.Substring($source.Length).TrimStart('\')
    $out = Join-Path $pack $relative
    New-Item -ItemType Directory $out -Force | Out-Null
    & $cli convert deserialize $_.FullName -o $out
    if ($LASTEXITCODE) { throw "couldn't convert $($_.FullName)" }
}
& $cli pack $pack -o $work
if ($LASTEXITCODE) { throw "packing failed" }
Copy-Item "$work\CyberCraft.archive" "$root\red4ext\archive\CyberCraft.archive" -Force
& $cli archive "$root\red4ext\archive\CyberCraft.archive" -l
