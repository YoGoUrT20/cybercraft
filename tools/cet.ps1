# Runs Lua in the running game through the CyberCraft dev console (tools\cet-devconsole, deployed as
# the CET mod "cybercraft_dev") and prints what it returned.
#
#   powershell -File tools\cet.ps1 "return Game.GetPlayer():GetWorldPosition()"
#   powershell -File tools\cet.ps1 -File probe.lua
param(
    [Parameter(Position = 0)][string]$Code,
    [string]$File,
    [int]$TimeoutSeconds = 10,
    [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077"
)
$ErrorActionPreference = "Stop"
$mod = Join-Path $Game "bin\x64\plugins\cyber_engine_tweaks\mods\cybercraft_dev"
if ($File) { $Code = Get-Content $File -Raw }
$id = [guid]::NewGuid().ToString("N").Substring(0, 12)
$out = Join-Path $mod "out.txt"
Set-Content -Path (Join-Path $mod "cmd.lua") -Value ("-- $id`n" + $Code) -NoNewline -Encoding utf8
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    if (Test-Path $out) {
        $text = Get-Content $out -Raw -ErrorAction SilentlyContinue
        if ($text -and $text.StartsWith("-- $id")) {
            $text.Substring($text.IndexOf("`n") + 1).TrimEnd()
            exit 0
        }
    }
    Start-Sleep -Milliseconds 100
}
Write-Error "no answer from the game in $TimeoutSeconds s (is it running, with CET and the cybercraft_dev mod?)"
