<#
.SYNOPSIS
    Install SFSE and/or the Address Library from archives you downloaded from Nexus Mods.

.DESCRIPTION
    SFSE (Nexus mod 106) and "Address Library for SFSE Plugins" (Nexus mod 3256)
    are only distributed through Nexus Mods, so download them in your browser
    first. This script then:
      - extracts each archive into a fresh temporary folder (7-Zip),
      - for SFSE: requires valid Authenticode signatures on every .exe/.dll and
        checks that the runtime DLL matches the installed Starfield.exe version,
      - for the Address Library: requires versionlib-<game version>-0.bin,
      - copies only the expected files into the game folder and lists them.
    Nothing is executed from the archives.

.EXAMPLE
    ./scripts/install-sfse.ps1 -SfseArchive ~/Downloads/sfse_0_2_21.7z -AddressLibraryArchive "~/Downloads/Address Library*.zip"
#>
param(
    [string]$SfseArchive,
    [string]$AddressLibraryArchive,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

function Find-GameDir {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { return $null }
    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"')) {
            $libraries += $m.Matches[0].Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in $libraries | Select-Object -Unique) {
        $candidate = Join-Path $lib 'steamapps\common\Starfield'
        if (Test-Path (Join-Path $candidate 'Starfield.exe')) { return $candidate }
    }
    return $null
}

function Find-7Zip {
    foreach ($c in @(
            (Get-Command 7z -ErrorAction SilentlyContinue).Source,
            "$env:ProgramFiles\7-Zip\7z.exe",
            (Get-ChildItem "$env:LOCALAPPDATA\vcpkg\downloads\tools" -Recurse -Filter 7z.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName)) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    throw '7-Zip not found. Install 7-Zip from https://7-zip.org/.'
}

function Expand-ToTemp([string]$archive) {
    $resolved = (Resolve-Path $archive -ErrorAction Stop | Select-Object -First 1).Path
    $dest = Join-Path ([IO.Path]::GetTempPath()) ("cosync-extract-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dest | Out-Null
    & (Find-7Zip) x -y "-o$dest" $resolved | Out-Null
    if ($LASTEXITCODE) { throw "failed to extract $resolved" }
    return $dest
}

if (-not $SfseArchive -and -not $AddressLibraryArchive) {
    throw 'Pass -SfseArchive and/or -AddressLibraryArchive (files downloaded from Nexus Mods).'
}
if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir) { throw 'Starfield not found; pass -GameDir.' }
$exeVersion = (Get-Item (Join-Path $GameDir 'Starfield.exe')).VersionInfo.FileVersion   # e.g. 1.16.244.0
$parts = $exeVersion.Split('.')
$underscore = "$($parts[0])_$($parts[1])_$($parts[2])"   # 1_16_244
$dashed = "$($parts[0])-$($parts[1])-$($parts[2])-0"     # 1-16-244-0
Write-Host "Starfield $exeVersion at $GameDir"
$plugins = Join-Path $GameDir 'Data\SFSE\Plugins'

if ($SfseArchive) {
    $src = Expand-ToTemp $SfseArchive
    try {
        $loader = Get-ChildItem $src -Recurse -Filter 'sfse_loader.exe' | Select-Object -First 1
        if (-not $loader) { throw 'archive does not contain sfse_loader.exe' }
        $root = $loader.DirectoryName
        $runtimeDll = Join-Path $root "sfse_$underscore.dll"
        if (-not (Test-Path $runtimeDll)) {
            $found = (Get-ChildItem $root -Filter 'sfse_1_*.dll' | ForEach-Object Name) -join ', '
            throw "this SFSE build does not support Starfield $exeVersion (has: $found)"
        }
        $binaries = Get-ChildItem $root -File | Where-Object { $_.Extension -in '.exe', '.dll' }
        foreach ($f in $binaries) {
            $sig = Get-AuthenticodeSignature $f.FullName
            if ($sig.Status -ne 'Valid') { throw "$($f.Name): signature $($sig.Status); refusing to install" }
            Write-Host ("  signed OK  {0,-24} {1}" -f $f.Name, $sig.SignerCertificate.Subject)
        }
        foreach ($f in $binaries) { Copy-Item $f.FullName -Destination $GameDir -Force; Write-Host "  installed  $($f.Name)" }
        $data = Join-Path $root 'Data'
        if (Test-Path $data) {
            Copy-Item $data -Destination $GameDir -Recurse -Force
            Write-Host '  installed  Data\ (SFSE scripts)'
        }
    } finally {
        Remove-Item $src -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($AddressLibraryArchive) {
    $src = Expand-ToTemp $AddressLibraryArchive
    try {
        $name = "versionlib-$dashed.bin"
        $bin = Get-ChildItem $src -Recurse -Filter $name | Select-Object -First 1
        if (-not $bin) {
            $found = (Get-ChildItem $src -Recurse -Filter 'versionlib-*.bin' | ForEach-Object Name) -join ', '
            throw "archive has no $name for Starfield $exeVersion (has: $found)"
        }
        New-Item -ItemType Directory -Force -Path $plugins | Out-Null
        Copy-Item $bin.FullName -Destination $plugins -Force
        Write-Host "  installed  Data\SFSE\Plugins\$name"
    } finally {
        Remove-Item $src -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$ok = (Test-Path (Join-Path $GameDir 'sfse_loader.exe')) -and (Test-Path (Join-Path $GameDir "sfse_$underscore.dll"))
$al = Test-Path (Join-Path $plugins "versionlib-$dashed.bin")
Write-Host ''
Write-Host "SFSE for $exeVersion          : $(if ($ok) { 'installed' } else { 'MISSING' })"
Write-Host "Address Library for ${exeVersion}: $(if ($al) { 'installed' } else { 'MISSING' })"
