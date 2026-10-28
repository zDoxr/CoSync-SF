<#
.SYNOPSIS
    Copy the built CoSyncSF.dll (+ .pdb) into Starfield\Data\SFSE\Plugins.

.DESCRIPTION
    An existing CoSyncSF.ini in the game folder is kept; a default one is only
    copied if none exists. scripts/launch-instances.ps1 uses per-instance
    configs under local\instances instead, so the game-folder INI only matters
    for normal single launches.

.EXAMPLE
    ./scripts/deploy-plugin.ps1                  # release build
    ./scripts/deploy-plugin.ps1 -Preset debug
#>
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'release',
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $repo "build\$Preset\stage\Data\SFSE\Plugins"
if (-not (Test-Path (Join-Path $stage 'CoSyncSF.dll'))) {
    throw "No staged plugin in $stage. Build first: ./scripts/build.ps1 -Preset $Preset"
}

if (-not $GameDir) {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"')) { $libraries += $m.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    $GameDir = $libraries | Select-Object -Unique | ForEach-Object { Join-Path $_ 'steamapps\common\Starfield' } |
        Where-Object { Test-Path (Join-Path $_ 'Starfield.exe') } | Select-Object -First 1
}
if (-not $GameDir) { throw 'Starfield not found; pass -GameDir.' }

if (Get-Process Starfield -ErrorAction SilentlyContinue) {
    throw 'Starfield is running; close it before deploying (the DLL is locked while loaded).'
}

$dest = Join-Path $GameDir 'Data\SFSE\Plugins'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($f in 'CoSyncSF.dll', 'CoSyncSF.pdb') {
    Copy-Item (Join-Path $stage $f) -Destination $dest -Force
    Write-Host "deployed $f -> $dest"
}
if (-not (Test-Path (Join-Path $dest 'CoSyncSF.ini'))) {
    Copy-Item (Join-Path $stage 'CoSyncSF.ini') -Destination $dest
    Write-Host 'deployed default CoSyncSF.ini (Mode=off)'
}

$version = (Get-Item (Join-Path $GameDir 'Starfield.exe')).VersionInfo.FileVersion.Split('.')
$addressLib = Join-Path $dest ("versionlib-{0}-{1}-{2}-0.bin" -f $version[0], $version[1], $version[2])
$sfse = Join-Path $GameDir ("sfse_{0}_{1}_{2}.dll" -f $version[0], $version[1], $version[2])
if (-not (Test-Path $sfse)) { Write-Warning 'SFSE is not installed for this game version; the plugin will not load. See scripts/install-sfse.ps1.' }
if (-not (Test-Path $addressLib)) { Write-Warning "Address Library missing ($([IO.Path]::GetFileName($addressLib))); SFSE will refuse to load the plugin. See scripts/install-sfse.ps1." }
