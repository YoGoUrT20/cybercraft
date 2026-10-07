# Builds both halves of CyberCraft and packs a release into dist\:
#   CyberCraft-<version>.zip           the Cyberpunk 2077 mod (unpack into the game folder, or install
#                                      with Vortex): the RED4ext plugin and CyberCraft-Minecraft.zip,
#                                      the Minecraft it starts
#   CyberCraftSymbols-<version>.zip    the plugin's debug symbols, for reading crash logs
#   cybercraft-fabric-<version>.jar      the Minecraft mod on its own (for your own launcher)
#   Codeware-<version>.zip, mod_settings_v<version>.zip
#                                      the unmodified releases of the two mods CyberCraft uses, as
#                                      Melty installs them alongside it (melty.json)
#
# CyberCraft-Minecraft.zip holds a portable Prism Launcher with a ready "CyberCraft" instance
# (Minecraft 26.3, Fabric, Fabric API, the mod). The plugin unpacks it to %LOCALAPPDATA%\CyberCraft
# and starts it; Prism asks the player to sign in once, then downloads Minecraft and Java itself.
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
# The plugin's version (red4ext\CMakeLists.txt) names the release; the Fabric mod keeps its own.
$version = (Select-String -Path "$root\red4ext\CMakeLists.txt" -Pattern 'project\(CyberCraft VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$modVersion = (Select-String -Path "$root\fabric\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()

# Pinned downloads (checked against these hashes).
$prismVersion = "11.1.1"
$prismZip = "PrismLauncher-Windows-MSVC-Portable-$prismVersion.zip"
$prismUrl = "https://github.com/PrismLauncher/PrismLauncher/releases/download/$prismVersion/$prismZip"
$prismSha256 = "ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7"
$prismLicenseUrl = "https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/$prismVersion/LICENSE"
$fabricApiJar = "fabric-api-0.161.0+26.3.jar"
$fabricApiUrl = "https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar"
$e4mcJar = "e4mc-fabric-6.2.2-modern.jar"
$e4mcUrl = "https://cdn.modrinth.com/data/qANg5Jrr/versions/AouleFRY/e4mc-fabric-6.2.2-modern.jar"
$e4mcSha512 = "01ef0a8c5b76e2cb0effd337bad3350d8807d100d0ec661e01b2ffb20af7b652f756c5eaa11bee233c37905bfd7b573f7a85f3d15bfd2833962c76f02cd59a86"
$fabricApiSha512 = "ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d"
# Shipped next to the release for Melty (melty.json), as their authors publish them (both MIT).
$codewareZip = "Codeware-1.20.5.zip"
$codewareUrl = "https://github.com/psiberx/cp2077-codeware/releases/download/v1.20.5/$codewareZip"
$codewareSha256 = "102989e199bad650fe6e53395c22bac53fdd7abecc6eeec3b0046886631591f0"
$modSettingsZip = "mod_settings_v0.2.21.zip"
$modSettingsUrl = "https://github.com/jackhumbert/mod_settings/releases/download/v0.2.21/$modSettingsZip"
$modSettingsSha256 = "7efa862a688e61002979de06ed0e7dcf0205f8d480c28aa91d8063721f27ce1f"

function Get-Pinned([string]$url, [string]$path, [string]$algorithm, [string]$hash) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory (Split-Path $path) -Force | Out-Null
        Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
    }
    if ($hash -and (Get-FileHash $path -Algorithm $algorithm).Hash -ne $hash.ToUpper()) {
        Remove-Item $path
        throw "$path doesn't match its pinned $algorithm hash"
    }
}

# Zip entries named explicitly with forward slashes, as the zip format (and every mod manager)
# expects; Windows PowerShell's own zipping writes backslashes.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function New-Zip([string]$path, [System.Collections.IDictionary]$entries) {
    $zip = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $entries.Keys) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $entries[$name], $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
}
function New-ZipFromFolder([string]$path, [string]$folder) {
    $entries = [ordered]@{}
    $base = (Resolve-Path $folder).Path.TrimEnd('\') + '\'
    Get-ChildItem $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entries[$_.FullName.Substring($base.Length).Replace('\', '/')] = $_.FullName
    }
    New-Zip $path $entries
}

if (-not $NoBuild) {
    Push-Location "$root\red4ext"
    try {
        cmake --preset default | Out-Null
        cmake --build --preset release
        if ($LASTEXITCODE) { throw "the RED4ext plugin didn't build" }
    } finally { Pop-Location }
    Push-Location "$root\fabric"
    try {
        .\gradlew.bat build --no-configuration-cache
        if ($LASTEXITCODE) { throw "the Fabric mod didn't build" }
    } finally { Pop-Location }
}

$dll = "$root\red4ext\build\RelWithDebInfo\CyberCraft.dll"
$pdb = "$root\red4ext\build\RelWithDebInfo\CyberCraft.pdb"
$jar = "$root\fabric\build\libs\cybercraft-$modVersion.jar"
foreach ($f in @($dll, $pdb, $jar)) {
    if (-not (Test-Path $f)) { throw "missing $f (build first, or drop -NoBuild)" }
}
$cache = "$root\.tools\prism"
Get-Pinned $prismUrl "$cache\$prismZip" SHA256 $prismSha256
Get-Pinned $fabricApiUrl "$cache\$fabricApiJar" SHA512 $fabricApiSha512
Get-Pinned $e4mcUrl "$cache\$e4mcJar" SHA512 $e4mcSha512
Get-Pinned $prismLicenseUrl "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "" ""
Get-Pinned $codewareUrl "$cache\$codewareZip" SHA256 $codewareSha256
Get-Pinned $modSettingsUrl "$cache\$modSettingsZip" SHA256 $modSettingsSha256

$dist = "$root\dist"
New-Item -ItemType Directory $dist -Force | Out-Null
Get-ChildItem $dist | Remove-Item -Recurse -Force

# The bundled Minecraft: Prism (portable), the CyberCraft instance, its mods, Prism's default settings.
$bundle = "$dist\bundle"
Copy-Item -Recurse "$root\tools\minecraft-bundle" $bundle
Expand-Archive "$cache\$prismZip" "$bundle\Prism" -Force
Copy-Item "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "$bundle\Prism\LICENSE-PrismLauncher.txt"
(Get-Content "$bundle\Prism\THIRD-PARTY.txt" -Raw).Replace("{PRISM_VERSION}", $prismVersion) | Set-Content "$bundle\Prism\THIRD-PARTY.txt" -NoNewline
$mods = "$bundle\Prism\instances\CyberCraft\.minecraft\mods"
New-Item -ItemType Directory $mods -Force | Out-Null
Copy-Item "$cache\$fabricApiJar" $mods
Copy-Item "$cache\$e4mcJar" $mods
Copy-Item $jar "$mods\cybercraft-$modVersion.jar"
Set-Content "$bundle\bundle-version.txt" "CyberCraft $version (mod $modVersion), Prism Launcher $prismVersion, $fabricApiJar, $e4mcJar" -NoNewline
New-ZipFromFolder "$dist\CyberCraft-Minecraft.zip" $bundle
Remove-Item -Recurse -Force $bundle

# Laid out from the game folder: RED4ext loads red4ext\plugins\<name>\*.dll, and the plugin looks for
# CyberCraft-Minecraft.zip (and CyberCraft.ini, if the player makes one) next to its DLL, and adds
# its scripts folder to the redscript compilation. The game loads archive\pc\mod\*.archive itself.
New-Zip "$dist\CyberCraft-$version.zip" ([ordered]@{
    "red4ext/plugins/CyberCraft/CyberCraft.dll" = $dll
    "red4ext/plugins/CyberCraft/CyberCraft-Minecraft.zip" = "$dist\CyberCraft-Minecraft.zip"
    "red4ext/plugins/CyberCraft/scripts/CyberCraft.reds" = "$root\red4ext\scripts\CyberCraft.reds"
    "red4ext/plugins/CyberCraft/scripts/CyberCraftModSettings.reds" = "$root\red4ext\scripts\CyberCraftModSettings.reds"
    "red4ext/plugins/CyberCraft/LICENSE.txt" = "$root\LICENSE"
    "red4ext/plugins/CyberCraft/THIRD-PARTY-NOTICES.md" = "$root\THIRD-PARTY-NOTICES.md"
    "archive/pc/mod/CyberCraft.archive" = "$root\red4ext\archive\CyberCraft.archive"
})
# Not CyberCraft-<version>-pdb.zip: melty.json's CyberCraft-*.zip would match it too.
New-Zip "$dist\CyberCraftSymbols-$version.zip" ([ordered]@{ "CyberCraft.pdb" = $pdb })
Copy-Item $jar "$dist\cybercraft-fabric-$modVersion.jar"
Copy-Item "$cache\$codewareZip", "$cache\$modSettingsZip" $dist
Remove-Item "$dist\CyberCraft-Minecraft.zip"

Get-ChildItem $dist | ForEach-Object { "{0,-40} {1,12:N0} bytes" -f $_.Name, $_.Length }
