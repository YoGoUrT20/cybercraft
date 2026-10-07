# Builds CyberCraft and installs it into Cyberpunk 2077, replacing whatever version is there: the
# plugin, its script, its archive and the Minecraft it starts. CyberCraft.ini is kept.
#
#   powershell -ExecutionPolicy Bypass -File tools\install.ps1 [-NoBuild] [-Game <Cyberpunk 2077 folder>] [-NoVortex]
#
# Installed with Vortex? Then the game's CyberCraft files are hardlinks into Vortex's staging folder
# (%APPDATA%\Vortex\cyberpunk2077\mods\CyberCraft-*). Both are updated, files rewritten in place so the
# links hold, and new files are linked the same way: Vortex keeps managing the mod, nothing to remove
# or drag in by hand. -NoVortex leaves Vortex's copy alone and writes the game folder only.
#
# The Minecraft side updates itself: the plugin unpacks CyberCraft-Minecraft.zip again (instance,
# mod jars) whenever the zip changes, on the next start of the game.
param(
    [switch]$NoBuild,
    [string]$Game,
    [switch]$NoVortex
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

function Find-Game {
    $candidates = @()
    if ($Game) { $candidates += $Game }
    if ($env:CYBERCRAFT_GAME) { $candidates += $env:CYBERCRAFT_GAME }
    $steam = $null
    try { $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath } catch {}
    if (-not $steam) { $steam = "C:\Program Files (x86)\Steam" }
    $candidates += Join-Path $steam "steamapps\common\Cyberpunk 2077"
    # Other Steam libraries: "path" lines of libraryfolders.vdf.
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($match in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $candidates += Join-Path ($match.Groups[1].Value -replace '\\\\', '\') "steamapps\common\Cyberpunk 2077"
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate "bin\x64\Cyberpunk2077.exe")) { return (Resolve-Path $candidate).Path }
    }
    throw "Cyberpunk 2077 not found; pass -Game <its folder>"
}

# Overwrites a file's contents without replacing the file, so hardlinks to it (Vortex's) see the new
# contents too.
function Write-InPlace([string]$path, [byte[]]$bytes) {
    New-Item -ItemType Directory (Split-Path $path) -Force | Out-Null
    $stream = [IO.File]::Open($path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}

function Get-Sha([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($bytes)) } finally { $sha.Dispose() }
}

function Get-FileSha([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    return Get-Sha ([IO.File]::ReadAllBytes($path))
}

$gameDir = Find-Game
"Cyberpunk 2077: $gameDir"
if (Get-Process -Name "Cyberpunk2077" -ErrorAction SilentlyContinue) {
    throw "Cyberpunk 2077 is running; close it first (its plugins can't be replaced while it runs)"
}

# The release zip, exactly what a player would install.
if ($NoBuild) { & "$PSScriptRoot\package.ps1" -NoBuild } else { & "$PSScriptRoot\package.ps1" }
$release = Get-ChildItem "$root\dist" -Filter "CyberCraft-*.zip" | Select-Object -First 1
if (-not $release) { throw "no release zip in $root\dist" }
"Installing $($release.Name)"

# Vortex's copy of the mod, if it installed one: the folder whose files the game's are linked to.
$staging = $null
if (-not $NoVortex) {
    $mods = Join-Path $env:APPDATA "Vortex\cyberpunk2077\mods"
    if (Test-Path $mods) {
        $staging = Get-ChildItem $mods -Directory | Where-Object { Test-Path (Join-Path $_.FullName "red4ext\plugins\CyberCraft\CyberCraft.dll") } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    }
    if ($staging) { "Vortex's copy: $($staging.FullName)" }
}

$zip = [IO.Compression.ZipFile]::OpenRead($release.FullName)
$installed = @{}
$added = 0; $updated = 0; $same = 0
try {
    foreach ($entry in $zip.Entries) {
        if ($entry.FullName.EndsWith("/")) { continue }
        $relative = $entry.FullName.Replace("/", "\")
        $installed[$relative.ToLowerInvariant()] = $true
        $reader = $entry.Open()
        $buffer = New-Object IO.MemoryStream
        try { $reader.CopyTo($buffer) } finally { $reader.Dispose() }
        $bytes = $buffer.ToArray()
        $sha = Get-Sha $bytes

        $stagingFile = $null
        $stagingChanged = $false
        if ($staging) {
            $stagingFile = Join-Path $staging.FullName $relative
            if ((Get-FileSha $stagingFile) -ne $sha) {
                Write-InPlace $stagingFile $bytes
                $stagingChanged = $true
            }
        }
        $gameFile = Join-Path $gameDir $relative
        if (-not (Test-Path $gameFile)) {
            if ($stagingFile) {
                New-Item -ItemType Directory (Split-Path $gameFile) -Force | Out-Null
                New-Item -ItemType HardLink -Path $gameFile -Target $stagingFile | Out-Null
            } else {
                Write-InPlace $gameFile $bytes
            }
            "  added    $relative"
            $added++
        } elseif ((Get-FileSha $gameFile) -ne $sha) {
            Write-InPlace $gameFile $bytes
            "  updated  $relative"
            $updated++
        } elseif ($stagingChanged) {
            # The game's file is Vortex's, linked: it changed with it.
            "  updated  $relative (through Vortex's link)"
            $updated++
        } else {
            "  same     $relative"
            $same++
        }
    }
} finally { $zip.Dispose() }

# What an older version left that this one doesn't ship: its DLLs, scripts, bundles and archives only.
# CyberCraft.ini and anything else are left alone.
$roots = @($gameDir)
if ($staging) { $roots += $staging.FullName }
foreach ($base in $roots) {
    $candidates = @()
    $plugin = Join-Path $base "red4ext\plugins\CyberCraft"
    if (Test-Path $plugin) {
        $candidates += Get-ChildItem $plugin -Recurse -File -Include *.dll, *.reds, *.zip, *.archive
    }
    $archives = Join-Path $base "archive\pc\mod"
    if (Test-Path $archives) {
        $candidates += Get-ChildItem $archives -File -Filter "CyberCraft*.archive"
    }
    foreach ($file in $candidates) {
        $relative = $file.FullName.Substring($base.Length).TrimStart('\')
        if (-not $installed.ContainsKey($relative.ToLowerInvariant())) {
            Remove-Item $file.FullName -Force
            "  removed  $relative ($base)"
        }
    }
}

""
"Done: $added added, $updated updated, $same already current."
"The next start of Cyberpunk 2077 unpacks the new Minecraft (keeping your sign-in and world)."
if ($staging) {
    "Vortex still manages CyberCraft. If it reports files changed outside Vortex, keep the changes."
}
